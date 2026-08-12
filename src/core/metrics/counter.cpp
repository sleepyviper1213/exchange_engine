#include "counter.hpp"

namespace exchange::core::metrics {

namespace detail {

void bump_relaxed(std::atomic<std::uint64_t> &slot,
				  std::uint64_t delta) noexcept {
	slot.store(slot.load(std::memory_order_relaxed) + delta,
			  std::memory_order_relaxed);
}

} // namespace detail

void counter::add(std::uint64_t delta) noexcept {
	detail::bump_relaxed(value_, delta);
}

void counter::increment() noexcept { add(1); }

std::uint64_t counter::load() const noexcept {
	return value_.load(std::memory_order_relaxed);
}

void counter::reset() noexcept { value_.store(0, std::memory_order_relaxed); }

} // namespace exchange::core::metrics
