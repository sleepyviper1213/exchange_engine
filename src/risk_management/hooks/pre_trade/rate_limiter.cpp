#include "rate_limiter.hpp"

#include <limits>

#ifdef __cpp_lib_saturation_arithmetic
#include <numeric>
#endif

namespace exchange::risk::hooks::pre_trade {
rate_limiter::rate_limiter(std::uint32_t max_per_window,
						   unsigned window_log2_ns) noexcept
	: limit_(max_per_window),
	  shift_(window_log2_ns <= MAX_WINDOW_LOG2_NS ? window_log2_ns
												  : MAX_WINDOW_LOG2_NS) {}

[[nodiscard]] std::uint32_t rate_limiter::limit() const noexcept {
	return limit_;
}

[[nodiscard]] std::uint64_t rate_limiter::window_ns() const noexcept {
	return std::uint64_t{1} << shift_;
}

[[nodiscard]] std::uint32_t
rate_limiter::used(monotonic_time now) const noexcept {
	// Still a shift, which is the whole design of this class - the count comes
	// out of the time_point once, here, and the path is unchanged.
	const auto epoch =
		static_cast<std::uint64_t>(now.time_since_epoch().count()) >> shift_;
	return used_ & -static_cast<std::uint32_t>(epoch == epoch_);
}

[[nodiscard]] std::uint32_t
rate_limiter::headroom(monotonic_time now) const noexcept {
	const std::uint32_t spent = used(now);
#ifdef __cpp_lib_saturation_arithmetic
	return std::saturating_sub(limit_, spent);
#else
	return limit_ < spent ? 0U : limit_ - spent;
#endif
}

[[nodiscard]] bool rate_limiter::admits(monotonic_time now,
										std::uint32_t count) const noexcept {
	return count <= headroom(now);
}

void rate_limiter::charge(monotonic_time now, std::uint32_t count) noexcept {
	constexpr std::uint32_t CEILING = std::numeric_limits<std::uint32_t>::max();

	used_  = used(now);
	epoch_ = static_cast<std::uint64_t>(now.time_since_epoch().count()) >> shift_;
	used_  = (used_ > CEILING - count) ? CEILING : used_ + count;
}

void rate_limiter::reset() noexcept {
	epoch_ = 0;
	used_  = 0;
}
} // namespace exchange::risk::hooks::pre_trade
