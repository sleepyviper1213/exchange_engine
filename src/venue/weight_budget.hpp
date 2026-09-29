#pragma once
// The venue's request allowance, counted in one place.
//
// Binance rate-limits by *IP address*, not by connection or by API key, over a
// rolling window - 6000 weight per minute on Spot at the time of writing, and
// every REST call has a documented weight (a depth snapshot at limit=100 costs
// 5, an order costs 1, /exchangeInfo costs 20). Exceeding it returns 429;
// continuing after a 429 returns 418 and bans the address for between two
// minutes and three days, escalating.
//
// So this is shared state, and that is the whole reason it lives in `venue/`
// rather than beside either caller. Market data polling snapshots and an order
// gateway placing orders spend from *the same* allowance; two components each
// tracking their own half would each stay under the limit and together sail
// past it. There is one budget because there is one address.
//
// Not thread-safe, and not lock-free: this is the millisecond-scale REST path,
// and the tree's answer to sharing is ownership - one budget lives with the
// coroutine that owns the connection, and anything else asks it. @see
// docs/directory_layout.md

#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <chrono>
#include <cstdint>
#include <vector>

namespace exchange::venue {

/// @brief Binance Spot's documented IP allowance per minute. @see weight_budget
inline constexpr int BINANCE_SPOT_WEIGHT_PER_MINUTE = 6000;

/**
 * @brief What a venue's rate limit has left, over a rolling window.
 *
 * @par Why buckets rather than a timestamp per request
 * A rolling window needs to know what falls out of it, and keeping one entry
 * per request would make that a list whose length is the request rate. Instead
 * the window is divided into one-second buckets in a ring, with a running
 * total: spending adds to the current bucket, and advancing the clock subtracts
 * and clears the buckets that have aged out. Both are O(seconds elapsed) with a
 * bound of the window, and the storage is fixed at construction.
 *
 * The cost is granularity: a window ending mid-second counts that whole second.
 * That errs towards *over*-counting recent spend, which is the safe direction -
 * it holds back slightly early rather than slightly late.
 *
 * @note Time is a parameter, never read from a clock inside. That keeps the
 *       module clock-free and the behaviour exactly testable; the caller
 *       already has a timestamp, because it just made a request.
 */
class weight_budget {
public:
	using clock      = std::chrono::steady_clock;
	using time_point = clock::time_point;

	/**
	 * @brief A budget of @p limit weight over @p window.
	 *
	 * @param limit Total weight admissible within the window. Non-positive is
	 *        read as "no limit", which @c can_spend then always allows.
	 * @param window The rolling period. Rounded up to whole seconds, and at
	 *        least one.
	 *
	 * @note This is the only allocation the class makes - one bucket per second
	 *       of the window, 240 bytes for Binance's minute - and it happens at
	 *       construction, which is startup. Nothing on the request path
	 *       allocates.
	 */
	VENUE_EXPORT explicit weight_budget(
		int limit                   = BINANCE_SPOT_WEIGHT_PER_MINUTE,
		std::chrono::seconds window = std::chrono::seconds{60});

	/// @brief Record @p weight spent at @p when.
	/// @note A weight of 0 still advances the window, which is why a caller
	///       that wants only to age the budget can spend nothing.
	VENUE_EXPORT void spend(int weight, time_point when) noexcept;

	/**
	 * @brief Replace the local count with the venue's own, as of @p when.
	 *
	 * Binance reports the running total on every response in
	 * @c X-MBX-USED-WEIGHT-1M, and that number is the truth where ours is an
	 * estimate: requests we retried, requests another process on this address
	 * made, and weights we guessed wrong all show up there and nowhere else.
	 * Adopting it is how the estimate stops drifting.
	 *
	 * @param used The venue's count for the current window.
	 * @param when When the response carrying it arrived.
	 * @note Collapses the window onto the present rather than redistributing
	 *       @p used across buckets - there is no information about when the
	 *       venue thinks it was spent. The effect is that the adopted total
	 *       ages out one full window from now, which again over-counts rather
	 *       than under-counts.
	 */
	VENUE_EXPORT void reconcile(int used, time_point when) noexcept;

	/// @brief Weight spent in the window ending at @p when.
	[[nodiscard]] VENUE_EXPORT int used(time_point when) noexcept;

	/// @brief Weight still admissible at @p when; never negative, and
	///        @c INT_MAX when the budget is unlimited.
	[[nodiscard]] VENUE_EXPORT int remaining(time_point when) noexcept;

	/// @brief Whether @p weight fits at @p when.
	/// @note Ask before sending, not after: the point of the budget is to be
	///       the thing that decides to wait, rather than the 429 that follows
	///       not having decided.
	[[nodiscard]] VENUE_EXPORT bool can_spend(int weight,
											  time_point when) noexcept;

	/// @brief The configured ceiling, as constructed.
	[[nodiscard]] VENUE_EXPORT int limit() const noexcept;

	/// @brief The rolling period, as constructed.
	[[nodiscard]] VENUE_EXPORT std::chrono::seconds window() const noexcept;

	/// @brief Whether this budget imposes any ceiling at all.
	[[nodiscard]] VENUE_EXPORT bool is_unlimited() const noexcept;

private:
	/// Age the ring forward to @p when, subtracting whatever left the window.
	void advance(time_point when) noexcept;

	int limit_ = 0;
	std::chrono::seconds window_{0};

	std::vector<std::int32_t> buckets_; ///< one per second of the window
	std::size_t at_     = 0;            ///< index of the current second
	std::int64_t total_ = 0;            ///< sum of @c buckets_, kept running

	/// The second @c buckets_[at_] represents. Absent until the first spend, so
	/// that a budget constructed long before its first use does not report a
	/// window that has already aged out.
	std::int64_t second_ = 0;
	bool started_        = false;
};

} // namespace exchange::venue
