#include "outcome_silence.hpp"

namespace exchange::risk::hooks::post_trade {

outcome_silence::outcome_silence(system::circuit_breaker &breaker,
								 const post_trade_limits &limits,
								 core::chrono::monotonic_time now) noexcept
	: breaker_(&breaker),
	  timeout_ns_(limits.outcome_timeout_ns),
	  last_outcome_(now) {}

void outcome_silence::beat(core::chrono::monotonic_time now) noexcept {
	last_outcome_ = now;
	++outcomes_;
}

bool outcome_silence::poll(core::chrono::monotonic_time now,
						   std::uint32_t working) noexcept {
	if (timeout_ns_ == NO_TIMEOUT) return false;
	if (!breaker_->passes_new_orders()) return false;
	if (!is_silent(now, working)) return false;

	breaker_->trip(system::trading_state::CANCEL_ONLY,
				   system::trip_cause::STALE_WORKING);
	++trips_;
	return true;
}

bool outcome_silence::is_silent(core::chrono::monotonic_time now,
								std::uint32_t working) const noexcept {
	return is_silent_with_exposure(silence_ns(now), working, timeout_ns_);
}

std::uint64_t
outcome_silence::silence_ns(core::chrono::monotonic_time now) const noexcept {
	// A reading behind the last one is not a rule violation: the clock is
	// monotonic, but a caller may poll with a `now` it took before the outcome
	// that has since arrived. Clamping to zero says "no silence", which is the
	// truth in that ordering, rather than wrapping to an enormous interval and
	// tripping the breaker on a race with itself.
	if (now <= last_outcome_) return 0;
	return static_cast<std::uint64_t>((now - last_outcome_).count());
}

core::chrono::monotonic_time outcome_silence::last_outcome() const noexcept {
	return last_outcome_;
}

std::uint64_t outcome_silence::timeout_ns() const noexcept {
	return timeout_ns_;
}

std::uint64_t outcome_silence::outcomes() const noexcept { return outcomes_; }

std::uint64_t outcome_silence::trips() const noexcept { return trips_; }

} // namespace exchange::risk::hooks::post_trade
