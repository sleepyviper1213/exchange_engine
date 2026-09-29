#pragma once
// Pricing a fill against a midpoint that has not happened yet.
//
// The awkward part of a markout is that it needs the future: a fill at t is
// scored against the mid at t + horizon, which the run has not reached. So this
// holds each fill until market time passes its longest horizon and scores it on
// the way through, rather than retaining the whole run and post-processing it.
// Memory is bounded by the fills inside one horizon window, not by the capture.

#include "fwd.hpp"
#include "markout_report.hpp"
#include "orders/types.hpp"
#include "strategy_export.hpp" // STRATEGY_EXPORT (generated)

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>

namespace exchange::strategy::backtest {

/**
 * @brief Scores fills against later midpoints, streaming, without look-ahead.
 *
 * @par The rule that makes the number honest
 * A horizon resolves against the last mid at or *before* its deadline - never
 * the first one after it. On a feed whose diffs arrive every 100ms that
 * distinction is most of the answer: scoring a 10ms horizon against the next
 * frame would price the fill with information from 90ms after the horizon
 * closed, which is the look-ahead bias a markout exists to detect. A deadline
 * falling between two frames therefore takes the earlier frame's mid, and a
 * deadline landing exactly on a frame takes that frame's.
 *
 * @par Feeding it
 * @c on_mid every time the replica moves and @c on_fill for every execution of
 * ours, both in market time. Order matters only in the obvious way: a fill is
 * scored against mids observed after it, so a mid and a fill bearing the same
 * stamp must arrive mid-first for that mid to count as the fill's own frame.
 * @c session drives both from the same event loop, so they do.
 *
 * @note Not thread-safe and not on any hot path - this is harness machinery
 *       that runs once per event and once per print.
 */
class markout_recorder {
public:
	/**
	 * @brief 10ms, 50ms, 100ms, 500ms, 1s, 5s.
	 *
	 * Spread over the cadences that matter rather than evenly: the first three
	 * straddle a 100ms diff feed's own frame time, which is where a passive
	 * fill is either stale or not, and the last two are long enough for the
	 * move that picked us off to have finished.
	 */
	static constexpr std::array<std::uint64_t, 6> DEFAULT_HORIZONS_NS{
		10'000'000ULL,
		50'000'000ULL,
		100'000'000ULL,
		500'000'000ULL,
		1'000'000'000ULL,
		5'000'000'000ULL};

	/// @brief A recorder over @c DEFAULT_HORIZONS_NS.
	STRATEGY_EXPORT markout_recorder();

	/**
	 * @brief A recorder over @p horizons_ns.
	 *
	 * @param horizons_ns Strictly ascending, positive, at most
	 *        @c markout_report::MAX_HORIZONS of them. Anything past the bound
	 *        is dropped and anything not ascending is a caller bug - the
	 *        pending queue resolves front-to-back and relies on the order.
	 */
	STRATEGY_EXPORT explicit markout_recorder(
		std::span<const std::uint64_t> horizons_ns);

	/**
	 * @brief Market time has reached @p now_ns and the midpoint is
	 *        @p mid_half_ticks.
	 *
	 * @param mid_half_ticks Best bid plus best ask, in ticks - *not* their
	 *        average. @see markout_report on why the mid travels doubled.
	 *
	 * @note Call only with a two-sided book. A one-sided replica has no
	 *       midpoint, and substituting the touch would score fills against a
	 *       price the market never showed; skip the frame instead. Time still
	 *       advances on the next frame that does have both sides, so a horizon
	 *       spanning the gap resolves against the last mid that existed.
	 */
	STRATEGY_EXPORT void on_mid(std::uint64_t now_ns,
								std::int64_t mid_half_ticks) noexcept;

	/**
	 * @brief Record one execution of ours at @p now_ns.
	 *
	 * @param our_side The side *we* were on - the resting side when the venue
	 *        aggressed, the aggressing side when we did. Getting this backwards
	 *        flips the sign of every markout, and both spellings type-check.
	 * @param price The execution price in ticks.
	 * @param volume Lots executed. Non-positive is ignored.
	 * @param is_passive Whether the venue came to us. @see session::absorb,
	 *        which already makes exactly this classification.
	 */
	STRATEGY_EXPORT void on_fill(std::uint64_t now_ns, side_t our_side,
								 price_t price, quantity_t volume,
								 bool is_passive);

	/**
	 * @brief The curve, with everything still pending counted as unresolved.
	 *
	 * @note Non-destructive, and does not invent a final mid. A fill whose
	 *       horizon the capture never reached is not scored against the last
	 *       frame that happened to exist - that would be a shorter horizon
	 *       wearing a longer one's name.
	 */
	[[nodiscard]] STRATEGY_EXPORT markout_report finish() const;

	/// @brief Fills still waiting on at least one horizon.
	[[nodiscard]] STRATEGY_EXPORT std::size_t pending() const noexcept;

	/// @brief Horizons this recorder measures.
	[[nodiscard]] STRATEGY_EXPORT std::size_t horizon_count() const noexcept;

private:
	/// @brief A fill waiting on one specific horizon.
	struct waiting {
		/// @brief When this horizon comes due - the fill's stamp plus it.
		std::uint64_t due_ns = 0;
		/// @brief Twice the execution price, pre-doubled to match the mid.
		std::int64_t price_half_ticks = 0;
		volume_t volume               = 0;
		bool is_passive               = false;
		bool is_buy                   = false;
	};

	/**
	 * @brief Score every deadline that has come due as of @p as_of at @p mid.
	 * @param inclusive Whether a deadline landing exactly on @p as_of counts.
	 */
	void resolve_due(std::uint64_t as_of, bool inclusive,
					 std::int64_t mid) noexcept;

	/// @brief Fold one scored horizon into @c totals_.
	void credit(const waiting &fill, std::size_t horizon,
				std::int64_t mid) noexcept;

	std::array<std::uint64_t, markout_report::MAX_HORIZONS> horizons_{};
	std::size_t horizon_count_ = 0;

	std::array<markout_report::bucket, markout_report::MAX_HORIZONS> totals_{};

	/**
	 * @brief One queue per horizon, each strictly ordered by deadline.
	 *
	 * Per horizon rather than one queue of fills, because a single queue cannot
	 * be drained from the front. Every entry in it would be at a different
	 * horizon index, and a later fill's next deadline can fall *before* an
	 * earlier fill's: with horizons of 10ms and 1s, a fill at t=0 that has
	 * already cleared 10ms is next due at 1s, while a fill at t=11ms is next
	 * due at 21ms. Stopping at the first entry that is not due would skip the
	 * second, and not stopping makes every frame a full scan. Split by horizon,
	 * each queue is sorted by construction and pops from the front.
	 */
	std::array<std::deque<waiting>, markout_report::MAX_HORIZONS> pending_{};

	std::int64_t last_mid_ = 0;
	bool has_mid_          = false;
};

} // namespace exchange::strategy::backtest
