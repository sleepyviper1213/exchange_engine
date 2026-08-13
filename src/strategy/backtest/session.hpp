#pragma once
// The harness: a recorded venue, this process's real engine, and one thread.
//
// Everything below the fill model is the shipped production path — the same
// depth_feed_bridge, the same risk_gate, the same engine_partition and
// matching_engine that a live deployment runs. A backtest that swapped any of
// them for a simulator would be testing the simulator.

#include "clock.hpp"
#include "depth_feed_bridge.hpp"
#include "fill_model.hpp"
#include "fwd.hpp"
#include "market-data/l2_book.hpp"
#include "market-data/normalised.hpp"
#include "market-data/reconstructor.hpp"
#include "market-data/sequencer.hpp"
#include "report.hpp"
#include "risk_management/circuit_breaker.hpp"
#include "risk_management/gate.hpp"
#include "risk_management/limits.hpp"
#include "risk_management/position.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/execution/book_manager.hpp"
#include "trading-engine/execution/engine_partition.hpp"
#include "trading-engine/execution/order_manager.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/symbol/symbol_spec.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace exchange::strategy::backtest {

/**
 * @brief What the harness drives — the same three entry points a strategy host
 *        already has.
 *
 * @c strategy_engine satisfies this as written, which is the point: the thing
 * under test in a backtest should be the thing that runs in production,
 * composed the same way, not a special offline variant of it.
 */
template <class T>
concept trader = requires(T &t, std::span<const engine::trade> trades,
						  std::span<const engine::order_outcome> outcomes) {
	{ t.on_trades(trades) } -> std::convertible_to<std::size_t>;
	{ t.on_outcomes(outcomes) } -> std::convertible_to<std::size_t>;
	{ t.flush() } -> std::same_as<bool>;
};

/**
 * @brief A trader that also wants to look at the market — an optional hook.
 *
 * Detected with a concept and elided with @c if @c constexpr, the way
 * @c strategy_engine treats its own streams. It exists because a backtest can
 * offer something a live strategy host cannot: the venue's reconstructed depth,
 * in the same process, for free. A quoter needs it and a trade-driven strategy
 * does not, so it is opt-in rather than part of @c trader.
 */
template <class T>
concept market_observer =
	requires(T &t, const market_data::l2_book &replica, std::uint64_t now_ns) {
		t.on_market(replica, now_ns);
	};

/**
 * @brief A trader that submits nothing.
 *
 * Replays a capture through the whole harness with no order flow — which checks
 * the harness rather than a strategy, and is exactly what the invariant "the
 * engine's book equals the venue's published depth" wants driving it.
 */
struct null_trader {
	std::size_t on_trades(std::span<const engine::trade> trades) noexcept {
		return trades.size();
	}

	std::size_t
	on_outcomes(std::span<const engine::order_outcome> outcomes) noexcept {
		return outcomes.size();
	}

	bool flush() noexcept { return true; }
};

static_assert(trader<null_trader>);
static_assert(!market_observer<null_trader>);

/// @brief How a @c session is put together.
struct session_options {
	/// @brief Passed to the reconstructor that gap-checks the recording.
	market_data::reconstructor_options feed{};
	/// @brief How generous the passive fill inference is.
	///        @see fill_model_options
	fill_model_options fills{};
	/// @brief The pre-trade policy. Defaults refuse nothing but the malformed,
	///        so a run with no limits configured measures the strategy rather
	///        than the gate. @see risk_limits
	risk::risk_limits limits{};
	/// @brief Resting-order hint for the listing's book.
	std::size_t book_capacity =
		engine::execution::book_manager::DEFAULT_BOOK_CAPACITY;
	/// @brief Records the partition's order store holds.
	std::uint32_t order_capacity =
		engine::execution::order_manager::DEFAULT_CAPACITY;
	/// @brief Breaches in one window that trip the breaker, or @c NO_AUTO_TRIP.
	std::uint32_t breaches_to_trip = risk::circuit_breaker::NO_AUTO_TRIP;

	/**
	 * @brief Most settle rounds one feed event may take.
	 *
	 * An event does not finish when its commands are applied: a fill can make a
	 * strategy quote again, the new quote may itself be crossed, and that fills
	 * too. The harness therefore iterates to a fixed point rather than running
	 * a fixed number of passes. This bounds the iteration so a strategy that
	 * replies to its own fills forever cannot hang the run — hitting it is
	 * reported as @c report::rounds_exhausted, and a run that hits it is not a
	 * clean replay.
	 */
	std::size_t max_rounds = 16;
};

/**
 * @brief One listing, one recording, one thread: the offline backtest.
 *
 * @par What it is
 * Feed it the normalised events of a capture and it drives the whole production
 * chain over them: the depth bridge turns the venue's published depth into
 * anonymous liquidity resting in a real @c order_book, a trader writes commands
 * into a real @c risk_gate, the gate delivers them to a real
 * @c engine_partition, and the matching engine executes them. What comes back
 * out is a @c report.
 *
 * @code
 * backtest::session run(spec, options);
 * backtest::spread_quoter quoter(run.sink(), spec);   // holds run.sink()
 * run.on_snapshot(seed, quoter);
 * for (auto &event : capture) run.on_event(std::move(event), quoter);
 * run.finish(quoter);
 * fmt::println("{}", backtest::report_summary{&run.result(), &spec});
 * @endcode
 *
 * @par Why the trader is a parameter of the methods and not of the class
 * A trader writes into @c sink(), which this object owns, so the session must
 * exist before the trader does — and if the session also owned the trader,
 * neither could be constructed first. Passing it per call breaks the cycle
 * without giving up static dispatch: the hooks are still direct calls the
 * compiler can inline, and an unimplemented @c on_market still compiles away.
 *
 * @par Why there is no consumer thread
 * A partition is single-producer, single-consumer, and in a deployment those
 * are two pinned threads. Here they are the same thread, taking turns: submit,
 * drain, publish, repeat. That is not a shortcut, it is the requirement — a
 * backtest's result must be a function of its input alone, and two threads
 * racing over a queue would make the interleaving, and therefore the fills, a
 * function of the machine. The code being exercised is identical either way;
 * only the scheduling is pinned down.
 *
 * @warning Not thread-safe, by construction. @see the note above.
 */
class session {
public:
	using command = engine::event::command;

	/**
	 * @brief Slots in the partition's command ring.
	 *
	 * Sized far above anything one settle round can put in it, because the loop
	 * drains at the top of every round: at no point does the ring hold more
	 * than one batch. Over-running it therefore means a single batch — a
	 * trader's whole buffer, or a full-depth resync — exceeded this on its own,
	 * which is a sizing fault rather than back-pressure, and is counted as
	 * @c report::commands_dropped rather than retried forever.
	 */
	static constexpr std::size_t QUEUE_CAPACITY = 1U << 14;

	using partition_type  = engine::execution::engine_partition<QUEUE_CAPACITY>;
	using fill_model_type = crossing_fill_model<partition_type>;
	using gate_type       = risk::risk_gate<fill_model_type, clock_view>;

	/**
	 * @brief Build a run for @p spec's listing.
	 * @param spec The listing's trading conventions. Must outlive the session —
	 *        the bridge, the fill model and the report all read it.
	 * @param options Sizing and policy. @see session_options
	 */
	explicit session(const engine::symbol_spec &spec,
					 session_options options = {})
		: options_(options),
		  spec_(&spec),
		  positions_(
			  std::max<std::size_t>(risk::position_book::DEFAULT_CAPACITY,
									static_cast<std::size_t>(spec.id()) + 1U)),
		  breaker_(options.breaches_to_trip),
		  partition_(partition_type::TradeSink{}, partition_type::OutcomeSink{},
					 options.book_capacity, options.order_capacity),
		  fills_(partition_, spec, options.fills),
		  gate_(fills_, spec.id(), options.limits, positions_, breaker_, 0,
				clock_view{clock_}),
		  bridge_(spec, options.feed) {
		partition_.listing(spec.id());
	}

	// The gate points at the fill model, the fill model at the partition, and a
	// trader at the gate. Nothing here may be relocated once those are bound.
	session(const session &)            = delete;
	session &operator=(const session &) = delete;
	session(session &&)                 = delete;
	session &operator=(session &&)      = delete;
	~session()                          = default;

	/// @brief Where a trader writes its commands: the risk gate, exactly as in
	/// a
	///        deployment. Everything it accepts reaches the matching engine.
	[[nodiscard]] gate_type &sink() noexcept { return gate_; }

	/**
	 * @brief Seed or repair the replica from a full-depth snapshot.
	 * @param snapshot The venue's depth; consumed.
	 * @param actor The trader. @see the class note on why it is a parameter.
	 * @return Whether the replica is live afterwards. @c false means the
	 *         snapshot predates the buffered events and a newer one is needed.
	 */
	template <trader Trader>
	bool on_snapshot(market_data::book_snapshot snapshot, Trader &actor) {
		++result_.snapshots;
		clock_.advance_to(
			static_cast<std::uint64_t>(snapshot.event_time.count()));
		feed_.clear();
		const bool live = bridge_.on_snapshot(std::move(snapshot), feed_);
		apply_feed(actor);
		settle(actor);
		return live;
	}

	/**
	 * @brief Advance the market by one recorded event, and run to quiescence.
	 *
	 * The whole of one frame's work: market time moves to the event's stamp,
	 * the bridge turns the new depth into ADD / REDUCE for the engine's book,
	 * the fill model asks what that depth must have executed against our
	 * resting orders, and the trader sees everything that resulted — repeating
	 * until nothing further happens.
	 *
	 * @param event The decoded, normalised diff; consumed.
	 * @param actor The trader.
	 * @return What the sequencer did with it. @c gap means the replica died and
	 *         the liquidity it had seeded has been withdrawn from the book.
	 */
	template <trader Trader>
	market_data::sequence_action on_event(market_data::depth_event event,
										  Trader &actor) {
		++result_.events_seen;
		const auto stamp = static_cast<std::uint64_t>(event.event_time.count());
		clock_.advance_to(stamp);
		if (stamp != 0) {
			if (result_.first_event_ns == 0) result_.first_event_ns = stamp;
			result_.last_event_ns = std::max(result_.last_event_ns, stamp);
		}

		feed_.clear();
		const market_data::sequence_action action =
			bridge_.on_event(std::move(event), feed_);
		switch (action) {
		case market_data::sequence_action::apply:
			++result_.events_applied;
			break;
		case market_data::sequence_action::buffer:
			++result_.events_buffered;
			break;
		case market_data::sequence_action::discard:
			++result_.events_discarded;
			break;
		case market_data::sequence_action::gap: ++result_.gaps; break;
		}

		apply_feed(actor);
		settle(actor);
		return action;
	}

	/**
	 * @brief Run out whatever the last event left in flight, and finalise the
	 *        report.
	 *
	 * The settle loop exits on the round that changed nothing, and a trader may
	 * have written commands *after* that round's drain — on the last event
	 * there would then be no later event to apply them. This is that later
	 * event. Call it once, after the capture is exhausted, before reading @c
	 * result().
	 */
	/// @note Does *not* release the fill model's liquidity budget. This is the
	///       tail of the last event, not a new one — releasing it would let a
	///       quote still resting through the venue's touch fill a second time
	///       against depth that has not moved since. @see
	///       crossing_fill_model::infer
	template <trader Trader>
	void finish(Trader &actor) {
		settle(actor);
		finalise();
	}

	/// @brief The run so far. Complete only after @c finish.
	[[nodiscard]] const report &result() const noexcept { return result_; }

	/// @brief The venue's reconstructed depth. Meaningful only while @c live().
	[[nodiscard]] const market_data::l2_book &replica() const noexcept {
		return bridge_.replica();
	}

	/// @brief Whether the replica is seeded and in sequence.
	[[nodiscard]] bool live() const noexcept { return bridge_.live(); }

	/// @brief The engine's book — the venue's depth as anonymous liquidity,
	/// plus
	///        whatever the trader has resting.
	[[nodiscard]] const engine::order_book &book() const noexcept {
		return *partition_.book(spec_->id());
	}

	/// @brief The bridge, for its feed-health and mirror counters.
	[[nodiscard]] const depth_feed_bridge &bridge() const noexcept {
		return bridge_;
	}

	/// @brief The partition, for its books and order records.
	[[nodiscard]] const partition_type &partition() const noexcept {
		return partition_;
	}

	/// @brief The gate, for its breach tallies and its mark.
	[[nodiscard]] const gate_type &gate() const noexcept { return gate_; }

	/// @brief The kill switch, for whether it tripped and why.
	[[nodiscard]] const risk::circuit_breaker &breaker() const noexcept {
		return breaker_;
	}

	/// @brief Market time, as of the last event. @see feed_clock
	[[nodiscard]] const feed_clock &clock() const noexcept { return clock_; }

	/// @brief The fill model, for what it has injected and still tracks.
	[[nodiscard]] const fill_model_type &fills() const noexcept {
		return fills_;
	}

private:
	// --- the loop -----------------------------------------------------------

	/// @brief Hand the bridge's depth commands to the engine and re-mark.
	template <trader Trader>
	void apply_feed(Trader &actor) {
		result_.depth_commands += feed_.size();
		submit_direct(feed_, actor);
		fills_.open_step();
		mark_to_market();
	}

	/**
	 * @brief Iterate until the event has finished happening.
	 *
	 * Order within a round is deliberate. The drain comes first, so the ring is
	 * empty before anything is submitted into it and back-pressure cannot arise
	 * from work this round is about to create. The market hook fires only on
	 * the first round, because a trader should see one market per event and not
	 * one per internal iteration. The fill model runs last, after the trader's
	 * new orders have been written but before they have been applied — so a
	 * quote placed this round is inferred against on the next, once the book
	 * actually holds it.
	 */
	template <trader Trader>
	void settle(Trader &actor) {
		int quiet = 0;
		for (std::size_t round = 0; round < options_.max_rounds; ++round) {
			bool progress = false;

			if (const std::size_t applied = partition_.drain(); applied > 0) {
				result_.commands_applied += applied;
				harvest(actor);
				progress = true;
			}

			if (round == 0)
				if constexpr (market_observer<Trader>)
					actor.on_market(bridge_.replica(), clock_.now_ns());

			if (!actor.flush()) ++result_.queue_stalls;
			if (collect_refusals(actor)) progress = true;

			injected_.clear();
			if (fills_.infer(bridge_.replica(),
							 partition_.orders(),
							 injected_) > 0) {
				submit_direct(injected_, actor);
				progress = true;
			}

			// Two quiet rounds, not one. A round's flush happens *after* its
			// drain, so commands the trader wrote this round are still on the
			// ring when the round ends and nothing observable has changed yet.
			// Stopping on the first quiet round would leave them there —
			// applied on the next event, and on the last event never. The
			// second quiet round is what proves the flush had nothing in it:
			// its drain comes back empty. There is no cheaper test, because a
			// flush that delivered nothing and one that delivered a batch both
			// return true.
			quiet = progress ? 0 : quiet + 1;
			if (quiet == 2) {
				fills_.retire_finished(partition_.orders());
				return;
			}
		}
		++result_.rounds_exhausted;
		fills_.retire_finished(partition_.orders());
	}

	/**
	 * @brief Put @p batch on the ring, draining if it somehow does not fit.
	 *
	 * @see QUEUE_CAPACITY on why the retry is a diagnostic rather than a
	 *      mechanism: one drain is enough or nothing is.
	 */
	template <trader Trader>
	void submit_direct(std::span<const command> batch, Trader &actor) {
		if (batch.empty()) return;
		if (partition_.submit_range(batch)) return;

		++result_.queue_stalls;
		if (const std::size_t applied = partition_.drain(); applied > 0) {
			result_.commands_applied += applied;
			harvest(actor);
		}
		if (partition_.submit_range(batch)) return;
		result_.commands_dropped += batch.size();
	}

	/**
	 * @brief Take what the drain produced, account for it, and publish it.
	 *
	 * The buffers are copied out first. They belong to the partition and are
	 * cleared by its next @c drain, and publishing can reach one — a trader
	 * whose sink is full is flushed against a ring that only a drain empties.
	 * Copying makes that safe instead of subtle; a backtest can afford it.
	 */
	template <trader Trader>
	void harvest(Trader &actor) {
		if (partition_.trades().empty() && partition_.outcomes().empty())
			return;
		trades_.assign(partition_.trades().begin(), partition_.trades().end());
		outcomes_.assign(partition_.outcomes().begin(),
						 partition_.outcomes().end());

		absorb();
		if (!trades_.empty()) actor.on_trades(trades_);
		if (!outcomes_.empty()) actor.on_outcomes(outcomes_);
	}

	/**
	 * @brief Attribute the batch: to the report, to the gate, and — where we
	 *        took the venue's liquidity — back to the bridge.
	 *
	 * @par Reading a trade's two ids
	 * The fill model injects the venue's side of a passive fill under the
	 * anonymous id, and @c depth_feed_bridge rests the venue's depth under it
	 * too. So a zero on the aggressor means the venue came to us, and a zero on
	 * the resting side means we went to it; between them they classify every
	 * print without anything having to be tagged. @see crossing_fill_model
	 */
	void absorb() {
		for (const engine::trade &print : trades_) {
			const bool venue_aggressed = print.aggressor == kAnonymous;
			const bool venue_rested    = print.resting == kAnonymous;

			if (venue_aggressed && !venue_rested) {
				++result_.passive_fills;
				result_.passive_lots += print.volume;
			} else if (!venue_aggressed && venue_rested) {
				++result_.aggressive_fills;
				result_.aggressive_lots += print.volume;
				// We consumed depth the venue is still publishing. Telling the
				// bridge is what keeps its mirror equal to the book; the next
				// diff then puts the liquidity back, which is the
				// no-market-impact assumption made explicit and time-delayed
				// rather than left as a silent shortfall the mirror never
				// repairs.
				const side_t taker = side_of(print.aggressor);
				bridge_.consumed(opposed(taker), print.price, print.volume);
				result_.depth_consumed_lots += print.volume;
			} else if (!venue_aggressed && !venue_rested) {
				++result_.self_fills;
			}
			// Both anonymous is unreachable: an injected aggressor can only
			// meet our own orders. @see crossing_fill_model on why.
		}

		gate_.on_trades(trades_);
		gate_.on_outcomes(outcomes_);
		count_outcomes(outcomes_);
	}

	/**
	 * @brief Publish what the gate refused, so a trader learns its order never
	 *        reached a book.
	 *
	 * @return Whether there was anything new to publish.
	 *
	 * @note Gated on the gate's cumulative @c refused() count rather than on
	 *       @c rejections() being non-empty. That buffer is cleared when the
	 *       gate is *next* handed a batch, not when it is read, so it stays
	 *       populated across every round in which the trader had nothing to
	 *       flush — and a settle loop reading it unconditionally would
	 * republish one refusal once per round and never reach a fixed point. The
	 *       counter only moves when a refusal actually happened.
	 */
	template <trader Trader>
	bool collect_refusals(Trader &actor) {
		const std::uint64_t refused = gate_.refused();
		if (refused == refused_seen_) return false;
		refused_seen_ = refused;
		if (gate_.rejections().empty()) return false;
		refusals_.assign(gate_.rejections().begin(), gate_.rejections().end());
		count_outcomes(refusals_);
		actor.on_outcomes(refusals_);
		return true;
	}

	void
	count_outcomes(std::span<const engine::order_outcome> records) noexcept {
		for (const engine::order_outcome &record : records) {
			switch (record.type) {
			case engine::OutcomeType::ACCEPTED:
				++result_.orders_accepted;
				break;
			case engine::OutcomeType::REJECTED:
				++result_.orders_rejected;
				break;
			case engine::OutcomeType::CANCELLED:
				++result_.orders_cancelled;
				break;
			case engine::OutcomeType::CANCEL_REJECTED:
				++result_.cancels_rejected;
				break;
			case engine::OutcomeType::FILL: break; // counted from the print
			}
		}
	}

	/// @brief Which side an order of ours joined, from the venue's own record.
	/// @pre @p id is not the anonymous sentinel.
	[[nodiscard]] side_t side_of(order_id_t id) const noexcept {
		const auto *record = partition_.orders().find_record(id);
		return record != nullptr ? record->side : side_t::bid;
	}

	/**
	 * @brief Mark the account to the replica's midpoint.
	 *
	 * The gate re-marks itself on every print, which is right in a live market
	 * where prints are continuous — and wrong here, where a strategy may hold a
	 * position for a whole capture without trading once. Its P&L would then be
	 * valued at the price of its own last fill, which is not a mark, it is a
	 * memory. Taking the midpoint of the venue's published book each event is
	 * the honest valuation and it costs one division per frame.
	 *
	 * Rounded down to the tick grid rather than converted: a midpoint sits off
	 * the grid whenever the spread is an odd number of ticks, and
	 * @c price_from_scaled rightly refuses that.
	 */
	void mark_to_market() {
		const auto bid = bridge_.replica().best_bid();
		const auto ask = bridge_.replica().best_ask();
		if (!bid.has_value() || !ask.has_value()) return;

		const auto tick = spec_->tick_scaled();
		const auto mid  = (*bid + *ask) / 2;
		if (mid <= 0 || tick <= 0) return;
		if (const auto ticks = spec_->price_from_scaled(mid - mid % tick))
			gate_.set_reference_price(*ticks);
	}

	/// @brief Copy the counters that live elsewhere into the report.
	///
	/// @note Re-marks first. The gate moves its reference price to every print,
	///       our own included, so without this a run's final P&L would be
	///       valued at the price of the last thing the strategy traded rather
	///       than at the last price the venue showed. @see mark_to_market
	void finalise() {
		mark_to_market();
		const auto snapshot   = positions_.snapshot(spec_->id());
		result_.net_lots      = snapshot.net_lots;
		result_.bought_lots   = snapshot.bought_lots;
		result_.sold_lots     = snapshot.sold_lots;
		result_.net_notional  = snapshot.net_notional;
		result_.mark          = gate_.reference_price();
		result_.pnl_tick_lots = gate_.pnl();

		result_.risk_refusals       = gate_.refused();
		result_.breaker_tripped     = !breaker_.passes_new_orders();
		result_.injected_aggressors = fills_.injected();

		result_.misroutes      = partition_.misrouted();
		result_.dropped_levels = bridge_.dropped_levels();
		result_.live_at_end    = bridge_.live();

		result_.clock_regressions = clock_.regressions();
	}

	/// @brief The reserved id everything the venue owns rests under.
	///        @see orders::order::id
	static constexpr order_id_t kAnonymous = 0;

	// Declaration order is load-bearing twice over: the gate binds a clock_view
	// onto clock_ and a pointer to fills_, and fills_ binds one to partition_.
	// Each must outlive what points at it.
	session_options options_;
	const engine::symbol_spec *spec_;
	feed_clock clock_;
	risk::position_book positions_;
	risk::circuit_breaker breaker_;
	partition_type partition_;
	fill_model_type fills_;
	gate_type gate_;
	depth_feed_bridge bridge_;

	report result_;
	/// The gate's refusal count as of the last time it was published.
	/// @see collect_refusals
	std::uint64_t refused_seen_ = 0;

	/// Reused across events so a run allocates only while these grow to their
	/// high-water mark.
	std::vector<command> feed_;
	std::vector<command> injected_;
	std::vector<engine::trade> trades_;
	std::vector<engine::order_outcome> outcomes_;
	std::vector<engine::order_outcome> refusals_;
};

} // namespace exchange::strategy::backtest
