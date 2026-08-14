#include "rate_limiter.hpp"

#include <limits>

namespace exchange::risk {
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
rate_limiter::used(std::uint64_t now_ns) const noexcept {
	const std::uint64_t epoch = now_ns >> shift_;
	return used_ & -static_cast<std::uint32_t>(epoch == epoch_);
}

[[nodiscard]] std::uint32_t
rate_limiter::headroom(std::uint64_t now_ns) const noexcept {
	const std::uint32_t spent = used(now_ns);
#ifdef __cpp_lib_saturation_arithmetic
	return std::saturating_sub(limit_, spent);
#endif
	return limit_ < spent ? 0U : limit_ - spent;
}

[[nodiscard]] bool rate_limiter::admits(std::uint64_t now_ns,
										std::uint32_t count) const noexcept {
	return count <= headroom(now_ns);
}

void rate_limiter::charge(std::uint64_t now_ns, std::uint32_t count) noexcept {
	const std::uint64_t epoch = now_ns >> shift_;
	std::uint32_t CEILING     = std::numeric_limits<std::uint32_t>::max();
	used_                     = used(now_ns);
	epoch_                    = epoch;
	used_ = (used_ > CEILING - count) ? CEILING : used_ + count;
}

void rate_limiter::reset() noexcept {
	epoch_ = 0;
	used_  = 0;
}
} // namespace exchange::risk
