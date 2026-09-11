#include "rate_limiter.hpp"

#include "core/util/saturating.hpp"
#include "risk_management/window.hpp" // window_of

#include <limits>

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
	return 1ull << shift_;
}

[[nodiscard]] std::uint32_t
rate_limiter::used(core::chrono::monotonic_time now) const noexcept {
	// Still a shift, which is the whole design of this class - and it is now
	// named, so this line says "which window is it" rather than showing how.
	// @see risk::window_of
	const std::uint64_t epoch = window_of(now, shift_);
	return used_ &
		   (std::uint32_t{0} - static_cast<std::uint32_t>(epoch == epoch_));
}

[[nodiscard]] std::uint32_t
rate_limiter::headroom(core::chrono::monotonic_time now) const noexcept {
	const std::uint32_t spent = used(now);
	return core::util::saturating_sub(limit_, spent);
}

[[nodiscard]] bool rate_limiter::admits(core::chrono::monotonic_time now,
										std::uint32_t count) const noexcept {
	return count <= headroom(now);
}

void rate_limiter::charge(core::chrono::monotonic_time now,
						  std::uint32_t count) noexcept {
	constexpr std::uint32_t CEILING = std::numeric_limits<std::uint32_t>::max();

	used_  = used(now);
	epoch_ = window_of(now, shift_);
	used_  = (used_ > CEILING - count) ? CEILING : used_ + count;
}

void rate_limiter::reset() noexcept {
	epoch_ = 0;
	used_  = 0;
}
} // namespace exchange::risk::hooks::pre_trade
