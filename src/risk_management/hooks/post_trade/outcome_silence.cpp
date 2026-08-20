#include "outcome_silence.hpp"

namespace exchange::risk::hooks::post_trade {

outcome_silence::outcome_silence(system::circuit_breaker &breaker,
								 const post_trade_limits &limits,
								 std::uint64_t now_ns) noexcept
	: breaker_(&breaker),
	  timeout_ns_(limits.outcome_timeout_ns),
	  last_outcome_ns_(now_ns) {}

void outcome_silence::beat(std::uint64_t now_ns) noexcept {
	last_outcome_ns_ = now_ns;
	++outcomes_;
}

bool outcome_silence::poll(std::uint64_t now_ns,
						   std::uint32_t working) noexcept {
	if (timeout_ns_ == NO_TIMEOUT) return false;
	if (!breaker_->passes_new_orders()) return false;
	if (!is_silent(now_ns, working)) return false;

	breaker_->trip(system::trading_state::CANCEL_ONLY,
				   system::trip_cause::STALE_WORKING);
	++trips_;
	return true;
}

bool outcome_silence::is_silent(std::uint64_t now_ns,
								std::uint32_t working) const noexcept {
	return is_silent_with_exposure(silence_ns(now_ns), working, timeout_ns_);
}

std::uint64_t outcome_silence::silence_ns(std::uint64_t now_ns) const noexcept {
	// A reading behind the last one is not a rule violation: the clock is
	// monotonic, but a caller may poll with a `now` it took before the outcome
	// that has since arrived. Clamping to zero says "no silence", which is the
	// truth in that ordering, rather than wrapping to an enormous interval and
	// tripping the breaker on a race with itself.
	return now_ns > last_outcome_ns_ ? now_ns - last_outcome_ns_ : 0;
}

std::uint64_t outcome_silence::last_outcome_ns() const noexcept {
	return last_outcome_ns_;
}

std::uint64_t outcome_silence::timeout_ns() const noexcept {
	return timeout_ns_;
}

std::uint64_t outcome_silence::outcomes() const noexcept { return outcomes_; }

std::uint64_t outcome_silence::trips() const noexcept { return trips_; }

} // namespace exchange::risk::hooks::post_trade
