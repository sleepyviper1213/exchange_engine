#pragma once
// What a fill turned out to be worth, a little while after it printed.
//
// `report` counts fills. This says whether they were any good: the midpoint at
// a set of horizons after each execution, signed so that positive is in our
// favour. A curve that slopes down as the horizon lengthens is adverse
// selection - we were filled because the market was about to move against us,
// not because we were early.
//
// Passive and aggressive are kept apart because only the passive half answers
// that question. Crossing the spread costs what it costs and the mid barely
// moves; resting and being picked off is the failure mode worth measuring.
//
// One flat aggregate of integers, for the reason report.hpp already gives: this
// is what gets diffed between two strategy revisions. @see format.hpp

#include "fwd.hpp"
#include "orders/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace exchange::strategy::backtest {

/**
 * @brief Size-weighted markout at each horizon, passive and aggressive apart.
 *
 * @par The unit is a half-tick
 * A midpoint sits off the tick grid whenever the spread is an odd number of
 * ticks, so the mid is carried *doubled* - @c best_bid + @c best_ask rather
 * than their average - and the execution price is doubled with it. Every
 * quantity below is therefore in half-ticks, and halving one gives ticks.
 * @c mark_to_market rounds a midpoint down to the grid because it has to store
 * a @c price_t; a markout must not, because the rounding is the same order of
 * magnitude as the thing being measured.
 *
 * @par Reading it
 * - @c passive_half_tick_lots against @c passive_lots is the average markout
 *   per lot. Divide by two for ticks.
 * - @c unresolved is fills whose horizon never arrived because the capture ran
 *   out. They are counted, never guessed - a bucket that is mostly unresolved
 *   is describing the first part of the run and nothing else.
 */
struct markout_report {
	/// @brief Most horizons a run may measure at once. Six are the default;
	///        the slack is for a study that wants a finer curve.
	static constexpr std::size_t MAX_HORIZONS = 8;

	/// @brief One horizon's totals.
	struct bucket {
		/// @brief How far after the fill the mid was read, in nanoseconds.
		std::uint64_t horizon_ns = 0;

		/// @brief Fills resolved at this horizon - the venue came to us.
		std::uint64_t passive_fills = 0;
		/// @brief Fills resolved at this horizon - we crossed to the venue.
		std::uint64_t aggressive_fills = 0;

		volume_t passive_lots    = 0;
		volume_t aggressive_lots = 0;

		/// @brief Σ markout × lots, in half-ticks. Signed; positive is profit.
		std::int64_t passive_half_tick_lots = 0;
		/// @brief Σ markout × lots, in half-ticks. Signed; positive is profit.
		std::int64_t aggressive_half_tick_lots = 0;

		/// @brief Fills this horizon outlived. @see the note above.
		std::uint64_t unresolved = 0;
	};

	/// @brief Ascending by horizon. Only the first @c count are populated.
	std::array<bucket, MAX_HORIZONS> horizons{};
	std::size_t count = 0;
};

/*
 * Derived readings, asked from outside, for the reason report.hpp states: a
 * bucket is a bag of independently-accumulated counters with no invariant for a
 * member function to speak for. Argument-dependent lookup finds these
 * unqualified.
 */

/// @brief Fills this horizon actually priced, both kinds together.
[[nodiscard]] constexpr std::uint64_t
resolved_fills(const markout_report::bucket &at) noexcept {
	return at.passive_fills + at.aggressive_fills;
}

/// @brief Lots this horizon actually priced, both kinds together.
[[nodiscard]] constexpr volume_t
resolved_lots(const markout_report::bucket &at) noexcept {
	return at.passive_lots + at.aggressive_lots;
}

/**
 * @brief Average passive markout per lot, in half-ticks, or 0 with no lots.
 *
 * Integer division, and deliberately: a markout reported to a precision the
 * tick grid does not have invites a reader to believe a difference that is
 * rounding. @c passive_half_tick_lots is there for anyone who wants the
 * remainder.
 */
[[nodiscard]] constexpr std::int64_t
passive_markout_per_lot(const markout_report::bucket &at) noexcept {
	return at.passive_lots > 0 ? at.passive_half_tick_lots / at.passive_lots
							   : 0;
}

/// @brief Average aggressive markout per lot, in half-ticks, or 0 with none.
[[nodiscard]] constexpr std::int64_t
aggressive_markout_per_lot(const markout_report::bucket &at) noexcept {
	return at.aggressive_lots > 0
			   ? at.aggressive_half_tick_lots / at.aggressive_lots
			   : 0;
}

/**
 * @brief Whether enough of this horizon resolved for its average to mean
 *        anything.
 *
 * Not a threshold on sample size - that is the reader's judgement - but on
 * *coverage*: a bucket where most fills outlived the capture is reporting a
 * biased subset, because the fills that resolved are exactly the early ones.
 */
[[nodiscard]] constexpr bool
is_covered(const markout_report::bucket &at) noexcept {
	const std::uint64_t seen = resolved_fills(at) + at.unresolved;
	return seen > 0 && at.unresolved * 2 < seen;
}

} // namespace exchange::strategy::backtest
