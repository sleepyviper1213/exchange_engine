#pragma once

#include "percentile.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>

namespace exchange::core::metrics {

/**
 * @brief The zero-based rank of the sample at @p p among @p count.
 *
 * @param p Which percentile. Already known to be a quantile in [0,1] and not a
 *        NaN, which is why nothing here re-checks it. @see percentile
 * @param count Samples available.
 * @return A rank in @c [0, count-1], or 0 when @p count is zero - which a
 *         caller must not index with. @see quantile_of, which handles that.
 *
 * @note @c q*n lands exactly on @c n at the top of the range, one past the last
 *       sample, so the result is folded back. That is the whole of the bounds
 *       handling, and it is arithmetic rather than input validation - the top
 *       of a range is not a bad argument.
 */
[[nodiscard]] constexpr std::size_t rank_of(percentile p,
											std::size_t count) noexcept {
	if (count == 0) return 0;
	const auto at =
		static_cast<std::size_t>(p.value() * static_cast<double>(count));
	return std::min(at, count - 1);
}

/**
 * @brief The value at percentile @p p of @p sorted.
 *
 * @param sorted Samples in ascending order. A precondition rather than
 *        something this arranges: sorting is the caller's, because every caller
 *        here has just sorted for its own reasons and a second pass would be
 *        pure waste.
 * @param p Which percentile.
 * @return The sample, or a value-initialised @c T when @p sorted is empty.
 *         Empty is not an error: a run that recorded nothing has no tail, and
 *         reporting zero is what every caller here already did.
 *
 * @note The precondition is asserted, which costs a linear scan under
 *       @c enable_hardening. Affordable exactly here and nowhere near the
 *       engine: the two callers are a replay report and a benchmark's summary,
 *       both of which run once at the end of a run over a range they have just
 *       walked anyway. The histogram's own path does not come through here -
 *       it reaches @c rank_of directly - so nothing on an ingest or matching
 *       path pays this.
 *
 *       Worth the scan because the failure it catches is silent: an unsorted
 *       range returns a real sample from a meaningless position, and a p99 that
 *       is merely *wrong* rather than obviously broken is the worst kind of
 *       number to publish.
 */
template <typename T>
[[nodiscard]] constexpr T quantile_of(std::span<const T> sorted,
									  percentile p) noexcept {
	if (sorted.empty()) return T{};
	assert(std::ranges::is_sorted(sorted) &&
		   "quantile_of needs its samples in ascending order");
	return sorted[rank_of(p, sorted.size())];
}

} // namespace exchange::core::metrics
