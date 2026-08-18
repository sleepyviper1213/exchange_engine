#pragma once
// The host: composes a set of strategies at compile time and runs them against
// what the engine published.
//
// It sits on the *other* side of the partition from the matching engine. A
// partition takes commands in and publishes trades and outcomes out; this takes
// those and turns them back into commands. Nothing here knows what a book is.

#include "command_writer.hpp"
#include "concepts.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>

namespace exchange::strategy {

/**
 * @brief Runs @p Strategies over one listing's published events, batching the
 *        commands they produce into @p Sink.
 *
 * @tparam Sink Where finished batches go - @c execution::engine_partition, or a
 *         test double. All-or-nothing @c submit_range. @see command_sink
 * @tparam Strategies The composition, by value. Each must declare
 *         @c MAX_COMMANDS_PER_EVENT and hook at least one stream.
 *
 * @par What "compile-time optimisation" buys, concretely
 * Three things, all decided during instantiation and none of them costing a
 * runtime branch:
 *
 * 1. **Unsubscribed streams disappear.** @c OBSERVES_TRADES and friends are fold
 *    expressions over the concepts. A stream nobody subscribed to compiles to
 *    `return true` - no loop, no tuple walk, no call. Feeding trades to a host
 *    of purely outcome-driven strategies is free, not cheap.
 * 2. **The buffer is sized by the composition.** @c COMMANDS_PER_EVENT is the
 *    sum of what the strategies promise, so the buffer is provably large enough
 *    for any one event and the capacity check moves off the write and onto the
 *    event boundary - one comparison against a constant per event instead of one
 *    per command, and nothing on the path can allocate.
 * 3. **Dispatch is direct.** Strategies are held by value in a tuple and reached
 *    through a fold, so every hook is a direct call the compiler can inline. No
 *    vtable, no @c std::function, no indirection to defeat the inliner.
 *
 * @par Back-pressure
 * The stream entry points return how many events they consumed. A short return
 * means the sink refused a batch - its queue is full - and the host stopped
 * rather than running on with less room than the bound requires. Nothing is
 * lost on either side: the pending commands are intact and the next @c flush
 * retries them, and the events that were not reached are still the caller's.
 * Recover by flushing until it succeeds, then feeding the unconsumed suffix:
 *
 * @code
 * std::span<const trade> left = batch;
 * while (!left.empty()) {
 *     left = left.subspan(host.on_trades(left));
 *     if (!left.empty() && !host.flush()) wait_for_the_consumer();
 * }
 * @endcode
 *
 * A caller that ignores the count is throwing away both the only signal that
 * the consumer has fallen behind and the record of where to resume.
 *
 * @par Threading
 * One host, one thread - the same thread that owns the producer side of the
 * sink, since that is what @c submit_range requires. The host itself holds no
 * synchronisation and needs none.
 *
 * @par One host, one listing
 * @c trade and @c order_outcome carry no symbol, so the events reaching a host
 * cannot be told apart by listing. The host is therefore constructed with one
 * and stamps it on everything its strategies emit. Two listings mean two hosts.
 */
template <class Sink, class... Strategies>
	requires command_sink<Sink> && (runnable_strategy<Strategies> && ...)
class strategy_engine {
public:
	/// @brief Worst-case commands one event can produce across the composition.
	static constexpr std::size_t COMMANDS_PER_EVENT =
		(std::size_t{0} + ... + Strategies::MAX_COMMANDS_PER_EVENT);

	/**
	 * @brief How many events' worth of commands accumulate before a flush.
	 *
	 * Purely an amortisation knob: correctness needs only room for one event.
	 * Sixteen keeps the buffer within a few cache lines for the strategies here
	 * while making the queue handshake - and its release store - a per-batch
	 * cost rather than a per-command one.
	 */
	static constexpr std::size_t EVENTS_PER_BATCH = 16;

	/// @brief Command slots the host reserves inline. Never grows.
	static constexpr std::size_t CAPACITY =
		COMMANDS_PER_EVENT * EVENTS_PER_BATCH > 0
			? COMMANDS_PER_EVENT * EVENTS_PER_BATCH
			: 1;

	/// @brief Whether any strategy subscribed to the trade stream.
	static constexpr bool OBSERVES_TRADES = (trade_observer<Strategies> || ...);

	/// @brief Whether any strategy subscribed to the outcome stream.
	static constexpr bool OBSERVES_OUTCOMES =
		(outcome_observer<Strategies> || ...);

	/// @brief Whether any strategy is driven by the clock.
	static constexpr bool OBSERVES_CLOCK = (clocked<Strategies> || ...);

	/**
	 * @brief Compose the strategies over @p symbol, publishing into @p sink.
	 * @param sink Must outlive the host; it is held by pointer, not owned.
	 * @param symbol The listing this host observes and addresses.
	 * @param strategies Moved into the host, which then owns them.
	 */
	explicit strategy_engine(Sink &sink, symbol_id_t symbol,
							 Strategies... strategies) noexcept
		: sink_(&sink),
		  strategies_(std::move(strategies)...),
		  batch_(symbol) {}

	// The batch's cursor points into the batch's own storage, so nothing here
	// can be relocated. A host lives on the thread that drains it.
	strategy_engine(const strategy_engine &)            = delete;
	strategy_engine &operator=(const strategy_engine &) = delete;
	strategy_engine(strategy_engine &&)                 = delete;
	strategy_engine &operator=(strategy_engine &&)      = delete;
	~strategy_engine()                                  = default;

	/**
	 * @brief Feed a batch of executions.
	 * @return How many were consumed. Short of @c trades.size() means the sink
	 *         refused a flush; feed the rest after one succeeds.
	 * @note Compiles to @c return @c trades.size() when no strategy observes
	 *       trades - the span is never walked and no strategy is touched.
	 */
	std::size_t on_trades(std::span<const engine::trade> trades) {
		if constexpr (!OBSERVES_TRADES) return trades.size();
		else
			return dispatch(trades, []<class S>(S &s, const engine::trade &t,
											    command_writer &out) {
				if constexpr (trade_observer<S>) s.on_trade(t, out);
			});
	}

	/**
	 * @brief Feed a batch of lifecycle records.
	 * @return How many were consumed. Short of @c outcomes.size() means the sink
	 *         refused a flush; feed the rest after one succeeds.
	 * @note Compiles to @c return @c outcomes.size() when no strategy observes
	 *       outcomes.
	 */
	std::size_t on_outcomes(std::span<const engine::order_outcome> outcomes) {
		if constexpr (!OBSERVES_OUTCOMES) return outcomes.size();
		else
			return dispatch(outcomes, []<class S>(S &s, const engine::order_outcome &o,
												  command_writer &out) {
				if constexpr (outcome_observer<S>) s.on_outcome(o, out);
			});
	}

	/**
	 * @brief Advance the clock to @p now_ns (nanoseconds since the epoch).
	 * @return @c false if the sink refused a flush; the pending batch is kept.
	 * @note Compiles to @c return @c true when no strategy is clocked.
	 */
	bool on_clock([[maybe_unused]] std::uint64_t now_ns) {
		if constexpr (!OBSERVES_CLOCK) return true;
		else {
			if (!reserve()) return false;
			std::apply(
				[&](Strategies &...s) {
					(invoke_clock(s, now_ns, batch_.writer()), ...);
				},
				strategies_);
			return true;
		}
	}

	/**
	 * @brief Hand whatever has accumulated to the sink and empty the buffer.
	 *
	 * Flushing an empty buffer succeeds without touching the sink, so a caller
	 * can flush unconditionally at the end of a loop iteration.
	 *
	 * @return @c false if the sink refused; the batch is left intact for a
	 *         later retry, and @c stalls() has counted the refusal.
	 */
	bool flush() {
		command_writer &out = batch_.writer();
		if (out.empty()) return true;
		const std::size_t count = out.size();
		if (!sink_->submit_range(out.written())) {
			++stalls_;
			return false;
		}
		out.reset();
		submitted_ += count;
		++batches_;
		return true;
	}

	/**
	 * @brief Make room for one event's worth of commands, flushing if needed.
	 *
	 * Call this before writing to @c writer() by hand - arming an iceberg,
	 * cancelling a parent, seeding a quote. The event entry points call it
	 * themselves.
	 *
	 * @return @c false if a flush was needed and the sink refused.
	 */
	[[nodiscard]] bool reserve() {
		if (batch_.writer().remaining() >= COMMANDS_PER_EVENT) return true;
		return flush();
	}

	/// @brief The cursor, for a caller driving a strategy directly. Only valid
	///        to write to after a successful @c reserve.
	[[nodiscard]] command_writer &writer() noexcept { return batch_.writer(); }

	/// @brief The @p I th strategy, in composition order.
	/// @note Spelled differently from @c get by necessity, not taste: one
	///       overload set taking either a @c std::size_t or a type cannot be
	///       called with a type argument, because overload resolution tries to
	///       parse @c iceberg<4> as a value and gives up before it gets there.
	template <std::size_t I>
	[[nodiscard]] auto &nth() noexcept {
		return std::get<I>(strategies_);
	}

	template <std::size_t I>
	[[nodiscard]] const auto &nth() const noexcept {
		return std::get<I>(strategies_);
	}

	/// @brief The one strategy of type @p S, when the composition has exactly
	///        one of it.
	template <class S>
	[[nodiscard]] S &get() noexcept {
		return std::get<S>(strategies_);
	}

	template <class S>
	[[nodiscard]] const S &get() const noexcept {
		return std::get<S>(strategies_);
	}

	/// @brief The listing this host observes.
	[[nodiscard]] symbol_id_t symbol() const noexcept {
		return batch_.writer().symbol();
	}

	/// @brief Commands written but not yet delivered.
	[[nodiscard]] std::size_t pending() const noexcept { return batch_.size(); }

	/// @brief Commands the sink has accepted since construction.
	[[nodiscard]] std::uint64_t submitted() const noexcept { return submitted_; }

	/// @brief Batches the sink has accepted since construction.
	[[nodiscard]] std::uint64_t batches() const noexcept { return batches_; }

	/**
	 * @brief Times the sink refused a batch.
	 *
	 * Not an error count - a full queue is the consumer telling the producer to
	 * wait, and the batch survived. It is a saturation signal: a host that
	 * stalls steadily is generating commands faster than its partition retires
	 * them, which no amount of retrying will fix.
	 */
	[[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

private:
	template <class Event, class Hook>
	std::size_t dispatch(std::span<const Event> events, Hook hook) {
		std::size_t consumed = 0;
		for (const Event &e : events) {
			// The watermark, and the only capacity check on the path: one
			// comparison against a compile-time constant per event, rather than
			// one per command written.
			if (!reserve()) break;
			std::apply(
				[&](Strategies &...s) { (hook(s, e, batch_.writer()), ...); },
				strategies_);
			++consumed;
		}
		return consumed;
	}

	// A lambda cannot be used for this one: on_clock's fold is over the pack
	// directly rather than over a span of events, so the per-strategy constexpr
	// test needs a named function template to hang `if constexpr` on.
	template <class S>
	static void invoke_clock(S &s, std::uint64_t now_ns,
							 command_writer &out) noexcept {
		if constexpr (clocked<S>) s.on_clock(now_ns, out);
	}

	Sink *sink_;
	std::tuple<Strategies...> strategies_;
	command_batch<CAPACITY> batch_;
	std::uint64_t submitted_ = 0;
	std::uint64_t batches_   = 0;
	std::uint64_t stalls_    = 0;
};

/**
 * @brief Compose a host, deducing the strategy types.
 *
 * @code
 * auto host = strategy::compose(partition, BTCUSD,
 *                               strategy::iceberg<>{10'000},
 *                               strategy::stop<>{});
 * @endcode
 *
 * @note Returns by value into a guaranteed-elision context - the host is
 *       immovable, so this only works as an initialiser, which is the only place
 *       it is wanted.
 */
template <class Sink, class... Strategies>
[[nodiscard]] auto compose(Sink &sink, symbol_id_t symbol,
						   Strategies... strategies) {
	return strategy_engine<Sink, Strategies...>(sink, symbol,
												std::move(strategies)...);
}

} // namespace exchange::strategy
