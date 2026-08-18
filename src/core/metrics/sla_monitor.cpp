#include "sla_monitor.hpp"

#include "histogram.hpp"

namespace exchange::core::metrics {

sla_monitor::sla_monitor(const histogram &target,
						 std::chrono::milliseconds interval,
						 breach_callback on_breach)
	: target_(target), interval_(interval), on_breach_(std::move(on_breach)) {
	// A lambda, not &sla_monitor::run directly: jthread only injects a
	// stop_token when invoking the callable as invoke(f, token, args...),
	// and for a pointer-to-member-function f that puts token where the
	// object argument belongs, so the well-formed call it would try is
	// (token.*run)(this) - never what's wanted. Wrapping in a lambda gives
	// jthread a plain callable it can call as f(token) and let run() take it
	// from there.
	worker_ = std::jthread(
		[this](const std::stop_token &token) { run(token); });
}

void sla_monitor::stop_monitoring() {
	worker_.request_stop();
	if (worker_.joinable()) worker_.join();
}

sla_monitor::~sla_monitor() { stop_monitoring(); }

void sla_monitor::check_now() {
	if (!target_.is_healthy()) on_breach_(target_);
}

void sla_monitor::run(const std::stop_token &token) {
	std::unique_lock lock(mutex_);
	for (;;) {
		// The stop_token overload registers a stop_callback that calls
		// notify_all() on this cv the instant request_stop() runs, so a
		// pending stop wakes this immediately rather than waiting out
		// whatever's left of interval_. The predicate is always false: there
		// is nothing else worth waking early for, so a real return of true
		// never happens and every wake is either the interval elapsing or a
		// stop - told apart by the explicit check below, not by this return
		// value.
		cv_.wait_for(lock, token, interval_, [] { return false; });
		if (token.stop_requested()) return;
		lock.unlock();
		check_now();
		lock.lock();
	}
}

} // namespace exchange::core::metrics
