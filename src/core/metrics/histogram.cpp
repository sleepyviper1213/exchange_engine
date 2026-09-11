#include "histogram.hpp"

#include "counter.hpp"
#include "percentile.hpp"
#include "quantile.hpp"

#include <bit>
#include <chrono>
#include <cstddef>
#include <limits>

namespace exchange::core::metrics {

void histogram::record(std::uint64_t value) noexcept {
	detail::bump_relaxed(buckets_[bucket_of(value)], 1);
}

std::uint64_t histogram::upper_bound(std::size_t index) noexcept {
	if (index == 0) return 0;
	if (index >= 64) return std::numeric_limits<std::uint64_t>::max();
	return (1ull << index) - 1;
}

std::uint64_t histogram::snapshot::quantile(percentile p) const noexcept {
	if (total == 0) return 0;
	// The rank comes from the shared rule rather than from an expression here,
	// so this and a quantile taken over a sorted vector cannot drift apart.
	// What differs below is only the *lookup*: a bucketed histogram has to walk
	// its cumulative counts to reach a rank an array would index. @see
	// quantile.hpp
	const std::size_t target = rank_of(p, static_cast<std::size_t>(total));
	std::size_t cumulative = 0;
	for (std::size_t i = 0; i < NUM_BUCKETS; ++i) {
		cumulative += static_cast<std::size_t>(counts[i]);
		if (cumulative > target ||
			cumulative == static_cast<std::size_t>(total))
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

bool histogram::is_healthy() const noexcept {
	const snapshot s = read();
	// The one place a sample meets a budget, and so the one place the histogram's
	// unit-free samples are read as nanoseconds. Spelled once, here, rather than
	// by giving record() a time type it has no business requiring.
	const auto within = [&](percentile q, std::chrono::nanoseconds budget) {
		return budget == std::chrono::nanoseconds::zero() ||
			   std::chrono::nanoseconds{s.quantile(q)} <= budget;
	};
	return within(percentile::P99, budgets_.p99) &&
		   within(percentile::P999, budgets_.p999) &&
		   within(percentile::PMAX, budgets_.max);
}

void histogram::reset() noexcept {
	for (auto &bucket : buckets_) bucket.store(0, std::memory_order_relaxed);
}

std::size_t histogram::bucket_of(std::uint64_t value) noexcept {
	return static_cast<std::size_t>(std::bit_width(value));
}

} // namespace exchange::core::metrics
