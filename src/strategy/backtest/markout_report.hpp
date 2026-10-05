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

#include "core/util/units_math.hpp"
#include "fwd.hpp"
#include "orders/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace exchange::strategy::backtest {

/// @brief A markout: how far the mid moved from a fill's price, signed, in the
///        unit a midpoint is exact in.
using half_ticks_t = mp_units::quantity<units::half_tick, std::int64_t>;

/// @brief Σ markout × lots - a markout weighted by the size it was earned on.
using half_tick_lots_t =
	mp_units::quantity<units::half_tick * units::lot, std::int64_t>;

/**
 * @brief A price in half-ticks - the grid on which the midpoint of any two
 *        prices is exact.
 *
 * On ticks, @c core::util::midpoint of a bid and an ask one tick apart rounds
 * toward the bid, and every odd spread puts the same half-tick bias into the
 * curve. On half-ticks both prices are even, so their midpoint is an integer
 * and nothing rounds: @c midpoint(in_half_ticks(bid), in_half_ticks(ask)).
 * 64-bit so doubling any @c price_t cannot overflow.
 */
using half_tick_price_t =
	mp_units::quantity_point<units::half_tick, units::price_zero, std::int64_t>;

/// @brief @p price on the half-tick grid: the same point, counted twice as
///        finely. Widened before the conversion doubles it.
[[nodiscard]] constexpr half_tick_price_t in_half_ticks(price_t price) noexcept {
	return mp_units::value_cast<std::int64_t>(price).in(units::half_tick);
}

/// @brief The midpoint of @p bid and @p ask with nothing rounded - what
///        @c markout_recorder::on_mid is fed. @see half_tick_price_t
[[nodiscard]] constexpr half_tick_price_t exact_mid(price_t bid,
													price_t ask) noexcept {
	return core::util::midpoint(in_half_ticks(bid), in_half_ticks(ask));
}

/**
 * @brief Size-weighted markout at each horizon, passive and aggressive apart.
 *
 * @par The unit is a half-tick
 * A midpoint sits off the tick grid whenever the spread is an odd number of
 * ticks, so the mid is carried in half-ticks - @c half_tick_price_t - and the
 * execution price is measured in the same unit. Every markout below is
 * therefore in half-ticks, and the type says so.
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

		volume_t passive_lots    = {};
		volume_t aggressive_lots = {};

		/// @brief Σ markout × lots. Signed; positive is profit.
		half_tick_lots_t passive_half_tick_lots = {};
		/// @brief Σ markout × lots. Signed; positive is profit.
		half_tick_lots_t aggressive_half_tick_lots = {};

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
[[nodiscard]] constexpr half_ticks_t
passive_markout_per_lot(const markout_report::bucket &at) noexcept {
	return mp_units::is_gt_zero(at.passive_lots)
			   ? half_ticks_t{at.passive_half_tick_lots / at.passive_lots}
			   : half_ticks_t{};
}

/// @brief Average aggressive markout per lot, in half-ticks, or 0 with none.
[[nodiscard]] constexpr half_ticks_t
aggressive_markout_per_lot(const markout_report::bucket &at) noexcept {
	return mp_units::is_gt_zero(at.aggressive_lots)
			   ? half_ticks_t{at.aggressive_half_tick_lots / at.aggressive_lots}
			   : half_ticks_t{};
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
