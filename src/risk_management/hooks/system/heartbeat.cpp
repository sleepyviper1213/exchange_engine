#include "heartbeat.hpp"

#include "trading_state.hpp"

#include <cstdint>

// Out of line for the same reason circuit_breaker's members are: nothing here
// is on the per-command path. A poll happens once per pass of an event loop and
// a beat once per frame, so a call through the module boundary costs neither of
// them anything measurable, and keeping the bodies here is what gives the file
// a translation unit rather than an inline definition in every consumer.

namespace exchange::risk::hooks::system {

heartbeat_monitor::heartbeat_monitor(circuit_breaker &breaker,
									 std::uint64_t timeout_ns,
									 std::uint64_t now_ns) noexcept
	: breaker_(&breaker), timeout_ns_(timeout_ns), last_beat_ns_(now_ns) {}

void heartbeat_monitor::beat(std::uint64_t now_ns) noexcept {
	last_beat_ns_ = now_ns;
	++beats_;
}

bool heartbeat_monitor::poll(std::uint64_t now_ns) noexcept {
	if (!is_silent(now_ns)) return false;
	// Not a breaker that is already open: one outage, one trip. An operator's
	// re-arm into a feed that is still silent does trip again, which is the
	// breaker's own documented contract rather than an oversight here.
	if (!breaker_->passes_new_orders()) return false;
	breaker_->trip(trading_state::CANCEL_ONLY, trip_cause::STALE_FEED);
	++trips_;
	return true;
}

[[nodiscard]] bool
heartbeat_monitor::is_silent(std::uint64_t now_ns) const noexcept {
	return timeout_ns_ != NO_TIMEOUT && silence_ns(now_ns) > timeout_ns_;
}

[[nodiscard]] std::uint64_t
heartbeat_monitor::silence_ns(std::uint64_t now_ns) const noexcept {
	return now_ns - last_beat_ns_;
}

[[nodiscard]] std::uint64_t heartbeat_monitor::last_beat_ns() const noexcept {
	return last_beat_ns_;
}

[[nodiscard]] std::uint64_t heartbeat_monitor::timeout_ns() const noexcept {
	return timeout_ns_;
}

[[nodiscard]] std::uint64_t heartbeat_monitor::beats() const noexcept {
	return beats_;
}

[[nodiscard]] std::uint64_t heartbeat_monitor::trips() const noexcept {
	return trips_;
}

} // namespace exchange::risk::hooks::system
