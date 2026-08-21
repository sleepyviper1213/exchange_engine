#pragma once
// What a run is worth reading afterwards.
//
// Deliberately one flat aggregate of integers rather than an object with
// behaviour: a report is the run's output, it is what gets diffed between two
// strategy revisions, and everything in it should be comparable by eye and by
// `diff`. No floating point anywhere - a P&L in tick-lots is exact, and the
// only place it becomes a decimal is at the point of printing. @see format.hpp

#include "fwd.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstdint>

namespace exchange::strategy::backtest {

/**
 * @brief Everything a backtest run measured.
 *
 * @par Read these three first
 * - @c gaps - a non-zero count means the replica died mid-run and the engine's
 *   seeded liquidity was withdrawn while it recovered. Every fill statistic
 *   below is then describing a market with holes in it.
 * - @c passive_fills against @c aggressive_fills - which half of the strategy
 *   the result actually came from, and therefore how much of it rests on the
 *   inference in fill_model.hpp rather than on the matching engine.
 * - @c rounds_exhausted - the settle loop hit its bound instead of reaching a
 *   fixed point. Commands were left unapplied; the run is not a clean replay.
 */
struct report {
	// --- the recording ------------------------------------------------------

	/// @brief Events handed to the harness.
	std::uint64_t events_seen = 0;
	/// @brief Events that reached the replica in sequence.
	std::uint64_t events_applied = 0;
	/// @brief Events held back waiting for a snapshot.
	std::uint64_t events_buffered = 0;
	/// @brief Events the sequencer dropped as already covered.
	std::uint64_t events_discarded = 0;
	/// @brief Times the replica died - a sequence gap, or a crossed book.
	std::uint64_t gaps = 0;
	/// @brief Snapshots fed in, seed included.
	std::uint64_t snapshots = 0;
	/// @brief Event stamps that would have moved market time backwards.
	std::uint64_t clock_regressions = 0;
	/// @brief First and last venue stamps, nanoseconds since the epoch.
	std::uint64_t first_event_ns = 0;
	std::uint64_t last_event_ns  = 0;
	/// @brief Whether the replica was live when the run ended.
	bool live_at_end = false;

	// --- the engine ---------------------------------------------------------

	/// @brief ADD / REDUCE the bridge emitted to keep the book equal to the
	///        replica. Book churn the feed cost the engine.
	std::uint64_t depth_commands = 0;
	/// @brief Level changes the listing's spec could not express. Should be
	///        zero; anything else means the spec and the feed disagree about
	///        the instrument. @see depth_feed_bridge::dropped_levels
	std::uint64_t dropped_levels = 0;
	/// @brief Commands the partition applied, everything included.
	std::uint64_t commands_applied = 0;
	/// @brief Commands naming a listing the partition does not carry. Should be
	///        zero. @see engine_partition::misrouted
	std::uint64_t misroutes = 0;
	/// @brief Times a submission found the command queue full.
	std::uint64_t queue_stalls = 0;
	/// @brief Commands a full ring refused twice and the harness gave up on.
	///        Should be zero; anything else is a sizing fault, and the run is
	///        missing work rather than merely slow. @see
	///        session::QUEUE_CAPACITY
	std::uint64_t commands_dropped = 0;
	/// @brief Events where the settle loop hit @c session_options::max_rounds.
	std::uint64_t rounds_exhausted = 0;
	/// @brief Commands of ours still on the wire when the capture ran out.
	///
	/// The tail of the modelled latency, not a fault: there is no market left
	/// to apply them against, so they are reported rather than delivered.
	/// Always zero when @c latency_model::order_entry_ns is. @see wire
	std::uint64_t commands_in_flight = 0;

	// --- our order flow -----------------------------------------------------

	std::uint64_t orders_accepted  = 0;
	std::uint64_t orders_rejected  = 0;
	std::uint64_t orders_cancelled = 0;
	std::uint64_t cancels_rejected = 0;
	/// @brief Commands the risk gate refused before the engine saw them. A
	///        subset of @c orders_rejected - the gate's share of it.
	std::uint64_t risk_refusals = 0;
	/// @brief Whether the circuit breaker was still tripped at the end.
	bool breaker_tripped = false;

	// --- executions ---------------------------------------------------------

	/// @brief Fills where the venue came to us - inferred by the fill model.
	std::uint64_t passive_fills = 0;
	/// @brief Fills where we crossed the venue's published depth. These went
	///        through the matching engine against real quoted size.
	std::uint64_t aggressive_fills = 0;
	/// @brief Prints where both sides were orders of ours. Not an error, but a
	///        strategy trading with itself is rarely what was intended.
	std::uint64_t self_fills = 0;
	volume_t passive_lots    = 0;
	volume_t aggressive_lots = 0;
	/// @brief Aggressing orders the fill model injected.
	///        @see crossing_fill_model
	std::uint64_t injected_aggressors = 0;
	/// @brief Venue depth our aggressive orders consumed and the model
	/// restored.
	///        The size of the no-market-impact assumption, made countable.
	volume_t depth_consumed_lots = 0;
	/// @brief Venue liquidity that filled orders queued ahead of ours instead
	///        of filling us.
	///
	/// The counterpart of the above: the size of the front-of-queue assumption
	/// we are *not* making. Read it against @c passive_lots - a run where it
	/// dwarfs them is a strategy that was quoting at prices somebody else
	/// owned. Zero when @c fill_model_options::model_queue_position is cleared.
	volume_t queue_absorbed_lots = 0;

	// --- the account --------------------------------------------------------

	volume_t net_lots         = 0; ///< signed; positive is long
	volume_t bought_lots      = 0;
	volume_t sold_lots        = 0;
	std::int64_t net_notional = 0; ///< signed cash spent, in tick-lots
	/// @brief Realised plus unrealised, in tick-lots, at @c mark.
	std::int64_t pnl_tick_lots = 0;
	/// @brief The price @c pnl_tick_lots was marked at, in ticks.
	price_t mark = 0;

};

/*
 * The four derived readings above, asked from outside. `report` is a bag of
 * independently-written counters - the session fills them in one field at a
 * time - so it has no invariant for a member function to speak for. These
 * compute, they do not guard, and saying so from outside keeps the record a
 * plain aggregate. Argument-dependent lookup finds them unqualified.
 */

/// @brief Executions against our orders, however they came about.
///
/// Named @c total_fills rather than @c fills because @c session::fills() already
/// means something else entirely - the fill *model* - and an unqualified free
/// function named @c fills would sit beside it in overload resolution. A member
/// can get away with the short name; a free one has to earn it.
[[nodiscard]] constexpr std::uint64_t total_fills(const report &run) noexcept {
	return run.passive_fills + run.aggressive_fills + run.self_fills;
}

/// @brief Lots we traded, passive and aggressive together.
[[nodiscard]] constexpr volume_t traded_lots(const report &run) noexcept {
	return run.passive_lots + run.aggressive_lots;
}

/// @brief Market time the run covered, in nanoseconds. Zero if the capture
///        carried no usable stamps.
[[nodiscard]] constexpr std::uint64_t covered_ns(const report &run) noexcept {
	return run.last_event_ns > run.first_event_ns
			   ? run.last_event_ns - run.first_event_ns
			   : 0;
}

/// @brief Whether anything happened that makes the numbers above suspect. Not
///        "did the strategy lose money" - that is a result, not a fault.
[[nodiscard]] constexpr bool is_clean(const report &run) noexcept {
	return run.gaps == 0 && run.dropped_levels == 0 && run.misroutes == 0 &&
		   run.rounds_exhausted == 0 && run.clock_regressions == 0 &&
		   run.commands_dropped == 0;
}

} // namespace exchange::strategy::backtest
