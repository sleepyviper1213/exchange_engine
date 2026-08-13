#include "histogram.hpp"

#include "counter.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace exchange::core::metrics {

void histogram::record(std::uint64_t value) noexcept {
	detail::bump_relaxed(buckets_[bucket_of(value)], 1);
}

std::uint64_t histogram::upper_bound(std::size_t index) noexcept {
	if (index == 0) return 0;
	if (index >= 64) return std::numeric_limits<std::uint64_t>::max();
	return (std::uint64_t{1} << index) - 1;
}

std::uint64_t histogram::snapshot::quantile(double q) const noexcept {
	if (total == 0) return 0;
	q = std::clamp(q, 0.0, 1.0);
	const auto target =
		static_cast<std::uint64_t>(q * static_cast<double>(total));
	std::uint64_t cumulative = 0;
	for (std::size_t i = 0; i < NUM_BUCKETS; ++i) {
		cumulative += counts[i];
		if (cumulative > target || cumulative == total)
			return upper_bound(i);
	}
	return upper_bound(NUM_BUCKETS - 1);
}

histogram::snapshot histogram::read() const noexcept {
	snapshot s;
	for (std::size_t i = 0; i < NUM_BUCKETS; ++i) {
		s.counts[i] = buckets_[i].load(std::memory_order_relaxed);
		s.total += s.counts[i];
	}
	return s;
}

void histogram::reset() noexcept {
	for (auto &bucket : buckets_) bucket.store(0, std::memory_order_relaxed);
}

std::size_t histogram::bucket_of(std::uint64_t value) noexcept {
	return static_cast<std::size_t>(std::bit_width(value));
}

} // namespace exchange::core::metrics
