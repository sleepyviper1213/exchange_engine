#pragma once

#include <cassert>
#include <cmath>
#include <compare> // IWYU pragma: keep - std::strong_ordering

namespace exchange::core::metrics {

/**
 * @brief A quantile in [0,1] - @c 0.99 for the 99th percentile.
 *
 * @note Named for percentiles because that is what the call sites say, and
 *       carries the quantile because that is what the arithmetic wants. The
 *       two differ by a factor of a hundred and the constants below are the
 *       only place the conversion is written down.
 *
 * @note Trivially copyable and the size of a @c double. It is passed by value
 *       everywhere, and a reference to one would be larger than the thing.
 */
class percentile {
public:
	/// @brief The median.
	static const percentile P50;

	/// @brief The 95th percentile.
	static const percentile P95;

	/// @brief The 99th percentile - the usual tail budget.
	///        @see histogram::latency_budgets
	static const percentile P99;

	/// @brief The 99.9th percentile.
	static const percentile P999;

	/// @brief The largest sample. Not a tail estimate: on a pinned thread the
	///        maximum is usually the OS preempting it rather than the code.
	///        @see docs/performance.md
	static const percentile PMAX;

	/**
	 * @brief A percentile at quantile @p q.
	 *
	 * @param q In [0,1]. Outside it, or a NaN, is a programming error rather
	 *        than an input to be handled - nothing in this tree derives a
	 *        percentile from a message or a flag, so the only way to get one
	 *        wrong is to write it wrong.
	 *
	 * @note The assertions are what let every reader of this value skip its own
	 *       range check, so they are the contract rather than scaffolding -
	 *       which is the posture @c enable_hardening already takes for the
	 *       tree, keeping @c assert live in optimised builds.
	 * Constant-evaluated at every use below, so a bad literal is a compile
	 * error.
	 */
	explicit constexpr percentile(double q) noexcept : q_(q) {
		assert(q == q && "quantile cannot be NaN");
		assert(q >= 0.0 && q <= 1.0 && "a percentile is a quantile in [0,1]");
	}

	/// @brief The quantile, in [0,1]. Never NaN. @see percentile()
	[[nodiscard]] constexpr double value() const noexcept { return q_; }

	/**
	 * @brief Ordering, so a caller can say which of two tails is further out.
	 *
	 * @c strong_ordering rather than the @c partial_ordering a defaulted
	 * @c <=> would give, and that is the invariant paying out again: a
	 * comparison between doubles is partial *only* because either might be a
	 * NaN, and neither of these can be.
	 *
	 * Written out rather than defaulted for a second reason too. A defaulted
	 * strong ordering over a @c double would have to distinguish @c -0.0 from
	 * @c +0.0 - @c std::strong_order does - and @c percentile{-0.0} is a legal
	 * way to spell zero here, since @c -0.0 @c >= @c 0.0 holds. Comparing
	 * numerically makes the two the same percentile, which is the only answer
	 * that agrees with @c operator==.
	 */
	[[nodiscard]] constexpr std::strong_ordering
	operator<=>(const percentile &other) const noexcept {
		if (q_ < other.q_) return std::strong_ordering::less;
		if (other.q_ < q_) return std::strong_ordering::greater;
		return std::strong_ordering::equal;
	}

	/// @brief Numeric equality, consistent with @c operator<=>.
	[[nodiscard]] constexpr bool
	operator==(const percentile &other) const noexcept {
		return q_ == other.q_;
	}

private:
	double q_;
};

// Defined out of line because a class cannot hold a static member of its own
// type while it is still incomplete. `inline` so the header carries them.
inline constexpr percentile percentile::P50{0.50};
inline constexpr percentile percentile::P95{0.95};
inline constexpr percentile percentile::P99{0.99};
inline constexpr percentile percentile::P999{0.999};
inline constexpr percentile percentile::PMAX{1.0};

} // namespace exchange::core::metrics
