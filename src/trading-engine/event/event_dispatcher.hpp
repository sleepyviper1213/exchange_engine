#pragma once
// The producer end of the return path: take published events off the channel and
// hand them to whoever is waiting for that listing.
//
// This is execution::dispatcher's mirror image, and the naming is deliberate.
// That one answers "which partition owns this symbol" on the way in; this one
// answers "who asked about this symbol" on the way out. Both are pure routing and
// both own no market state — but they live in different modules, because routing a
// command decides *where execution happens* while routing an event only decides
// *who is told*, and this module is communication. Neither knows anything about
// what sits on the far side of its question: this one reaches its consumers
// through a concept, so nothing here names `strategy/` or `risk_management/` and
// the module graph keeps pointing downward.

#include "engine_event.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::engine::event {

/**
 * @brief Somewhere published events come from — @c event_channel, or a test
 *        double.
 *
 * Only the host-side half of the channel's surface, so a double needs one method
 * and not four, and so nothing in this header can accidentally reach for the
 * engine-side methods it has no right to call.
 */
template <class S>
concept event_source = requires(S &s, std::span<engine_event> out) {
	{ s.receive(out) } -> std::convertible_to<std::size_t>;
};

/**
 * @brief Whoever the events are for: one object that knows every listing.
 *
 * The symbol is a parameter here even though @c strategy_engine and @c risk_gate
 * are each built for a single listing, because the mapping from listing to
 * consumer is a deployment's business and not this module's — a handler is the
 * adapter that owns it, and in a one-listing process it is a handler that ignores
 * the argument.
 *
 * @par Why both hooks return a count
 * Because @c strategy_engine's do, and for its reason: a host that turns events
 * back into commands can fill its outbound queue and have to stop mid-batch. A
 * short return is how it says so, and it is the only way this loop can know to
 * keep the rest rather than drop it. A handler that cannot refuse — the risk gate
 * only updates position, so it consumes everything — returns the size it was
 * given, which compiles to the same thing as no back-pressure at all.
 */
template <class H>
concept event_handler =
	requires(H &h, symbol_id_t symbol, std::span<const engine::trade> trades,
			 std::span<const engine::order_outcome> outcomes) {
		{ h.on_trades(symbol, trades) } -> std::convertible_to<std::size_t>;
		{ h.on_outcomes(symbol, outcomes) } -> std::convertible_to<std::size_t>;
	};

/**
 * @brief Drains an @c event_source and delivers each listing's events, in order,
 *        to an @c event_handler.
 *
 * @tparam Source Where events come from. @see event_source
 * @tparam Handler Who they are for. @see event_handler
 * @tparam BatchSize Events taken per @c pump, and the size of the three buffers
 *         below.
 *
 * @par What a pump does
 * One dequeue of up to @p BatchSize events, then a walk over that buffer cutting
 * it into maximal runs of same-listing, same-kind events and delivering each run
 * as one span. Runs, not individual events, because the consumers' entry points
 * are span-shaped: @c strategy_engine checks its command-buffer capacity once per
 * event rather than once per command precisely so that a batch amortises, and
 * feeding it one event at a time would throw that away.
 *
 * Runs are long in practice. A partition drains a queue that is mostly one
 * listing at a time, and @c event_channel stages a listing's trades ahead of its
 * outcomes, so the common batch is a handful of runs and not a hundred.
 *
 * @par Why the events are copied out of the buffer
 * A run in the dequeue buffer is a run of @c engine_event — 8 bytes of routing
 * header in front of each payload — and the handler wants @c span<const trade>.
 * Contiguous payloads and a per-event routing key are not simultaneously
 * satisfiable in one buffer, so a run is gathered into a typed staging array
 * before the call. It is a 24-byte trivially copyable move per event out of a
 * line that was just touched, on the return path rather than the matching path.
 * The alternative shapes all cost more: per-symbol rings multiply the queue
 * count by the listing count, and a per-event handler call gives up the
 * amortisation the span interface exists for.
 *
 * @par Back-pressure and where a stall resumes
 * If a handler consumes only part of a run, the pump stops there and keeps the
 * rest — the buffer and a cursor into it survive the call, and the next @c pump
 * delivers the remainder before dequeuing anything new. Nothing is lost and
 * nothing is reordered, which is the same contract @c strategy_engine documents
 * for its own short returns, one link further up the chain.
 *
 * @par Allocation
 * None, ever. The three buffers are @c std::array members sized by @p BatchSize:
 * @c BatchSize * (sizeof(engine_event) + sizeof(trade) + sizeof(order_outcome)),
 * which at the default is a few kilobytes and stays resident. The two staging
 * arrays are separate rather than a union of the two, which would halve that —
 * only one is ever live — at the cost of switching a union's active member on the
 * hot path for a saving that does not change which cache level this sits in.
 *
 * @par Threading
 * One dispatcher, one thread: the channel's consumer side, which is also the
 * thread the handler and its strategies live on. Nothing here synchronises,
 * because the channel already did.
 */
template <event_source Source, event_handler Handler,
		  std::size_t BatchSize = DEFAULT_EVENT_BATCH>
class event_dispatcher {
public:
	// A pump that could take nothing would deliver nothing, forever.
	static_assert(BatchSize > 0, "BatchSize must be positive");

	/// @brief Events taken per @c pump.
	static constexpr std::size_t BATCH_SIZE = BatchSize;

	/// @brief Route @p source's events to @p handler. Both must outlive the
	///        dispatcher; neither is owned.
	event_dispatcher(Source &source, Handler &handler) noexcept
		: source_(&source), handler_(&handler) {}

	// The cursor indexes this object's own buffer, so a dispatcher cannot be
	// relocated mid-stall without stranding a partly-delivered batch.
	event_dispatcher(const event_dispatcher &)            = delete;
	event_dispatcher &operator=(const event_dispatcher &) = delete;
	event_dispatcher(event_dispatcher &&)                 = delete;
	event_dispatcher &operator=(event_dispatcher &&)      = delete;
	~event_dispatcher()                                   = default;

	/**
	 * @brief Deliver one batch: finish any stalled one, then take a new one.
	 * @return How many events reached the handler during this call, stalled
	 *         leftovers included. Zero means either the channel was empty or the
	 *         handler is still refusing — @c is_stalled() tells the two apart, and
	 *         they call for different responses (wait for the engine, versus
	 *         drain whatever the handler is blocked on).
	 */
	std::size_t pump() {
		const std::uint64_t before = delivered_;
		if (!deliver()) return delivered_ - before;

		const std::size_t taken = source_->receive(std::span(batch_));
		size_                   = taken;
		cursor_                 = 0;
		if (taken == 0) return delivered_ - before;

		++pumps_;
		deliver();
		return delivered_ - before;
	}

	/**
	 * @brief Pump until the channel stops yielding events or the handler stalls.
	 * @return How many events reached the handler in total.
	 * @note Bounded by what the channel holds *now*: each pump takes only what a
	 *       single dequeue saw, so this terminates against a producer that is
	 *       still publishing rather than chasing it.
	 */
	std::size_t pump_all() {
		std::size_t total = 0;
		for (std::size_t n = pump(); n != 0; n = pump()) {
			total += n;
			if (is_stalled()) break;
		}
		return total;
	}

	/// @brief Whether a batch is part-delivered because the handler refused the
	///        rest. The next @c pump resumes it before taking anything new.
	[[nodiscard]] bool is_stalled() const noexcept { return cursor_ < size_; }

	/// @brief Events held back by a stall, waiting for the handler to take them.
	[[nodiscard]] std::size_t backlog() const noexcept {
		return size_ - cursor_;
	}

	/// @brief Events handed to the handler since construction.
	[[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_; }

	/// @brief Batches dequeued since construction — the amortisation
	///        denominator.
	[[nodiscard]] std::uint64_t pumps() const noexcept { return pumps_; }

	/// @brief Times a handler refused part of a run. @see event_channel::stalls
	[[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

private:
	/**
	 * @brief Deliver from @c cursor_ to the end of the buffer, run by run.
	 * @return @c true when the buffer is exhausted; @c false on a stall, with
	 *         @c cursor_ left on the first event the handler did not take.
	 */
	bool deliver() {
		while (cursor_ < size_) {
			const symbol_id_t symbol = batch_[cursor_].symbol;
			const EventKind kind     = batch_[cursor_].kind;

			std::size_t run = 1;
			while (cursor_ + run < size_ &&
				   batch_[cursor_ + run].symbol == symbol &&
				   batch_[cursor_ + run].kind == kind)
				++run;

			const std::size_t taken = kind == EventKind::TRADE
										  ? deliver_trades(symbol, run)
										  : deliver_outcomes(symbol, run);
			assert(taken <= run &&
				   "a handler cannot consume what it was not given");
			cursor_ += taken;
			delivered_ += taken;
			if (taken < run) [[unlikely]] {
				++stalls_;
				return false;
			}
		}
		size_   = 0;
		cursor_ = 0;
		return true;
	}

	std::size_t deliver_trades(symbol_id_t symbol, std::size_t run) {
		for (std::size_t i = 0; i < run; ++i)
			trade_stage_[i] = batch_[cursor_ + i].as_trade();
		return handler_->on_trades(
			symbol, std::span<const engine::trade>(trade_stage_.data(), run));
	}

	std::size_t deliver_outcomes(symbol_id_t symbol, std::size_t run) {
		for (std::size_t i = 0; i < run; ++i)
			outcome_stage_[i] = batch_[cursor_ + i].as_outcome();
		return handler_->on_outcomes(
			symbol,
			std::span<const engine::order_outcome>(outcome_stage_.data(), run));
	}

	Source *source_;
	Handler *handler_;

	std::array<engine_event, BatchSize> batch_{}; ///< what the last dequeue took
	std::size_t size_   = 0; ///< live prefix of batch_
	std::size_t cursor_ = 0; ///< first event in batch_ not yet delivered

	/// Reused per run, so a handler sees contiguous payloads. Only one of the two
	/// holds anything at a time; see the class note on why they are not a union.
	std::array<engine::trade, BatchSize> trade_stage_{};
	std::array<engine::order_outcome, BatchSize> outcome_stage_{};

	std::uint64_t delivered_ = 0;
	std::uint64_t pumps_     = 0;
	std::uint64_t stalls_    = 0;
};

/**
 * @brief Build a dispatcher, deducing both ends.
 *
 * @code
 * event::event_channel<> channel;
 * my_handler handler{...};
 * auto route = event::route_events(channel, handler);
 * while (running) route.pump_all();
 * @endcode
 *
 * @note Returns by value into a guaranteed-elision context — the dispatcher is
 *       immovable, so this only works as an initialiser, which is the only place
 *       it is wanted. @see strategy::compose, which is shaped the same way and
 *       for the same reason.
 */
template <std::size_t BatchSize = DEFAULT_EVENT_BATCH, class Source,
		  class Handler>
	requires event_source<Source> && event_handler<Handler>
[[nodiscard]] auto route_events(Source &source, Handler &handler) noexcept {
	return event_dispatcher<Source, Handler, BatchSize>(source, handler);
}

} // namespace exchange::engine::event
