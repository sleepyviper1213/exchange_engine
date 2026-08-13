#pragma once
// A background watch over one histogram's tail-latency budgets.
//
// histogram::is_healthy() answers "right now, are we within budget?" but
// something has to keep asking. A one-shot caller — the end of a run, the
// end of a benchmark — asks once and finds out too late to matter. This is
// the thing that asks on a timer instead, so a budget breach is a warning
// while the run is still in flight rather than a line in a summary after it.
//
// Deliberately not a hot-path type: it owns a std::jthread and wakes on an
// interval measured in milliseconds, an eternity next to anything
// core/metrics.hpp's histogram and counter are budgeted for. That is what
// makes a std::function callback and a condition_variable_any acceptable
// here when neither would be on the path they watch.

#include "core_export.hpp" // CORE_EXPORT (generated)
#include "fwd.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace exchange::core::metrics {

/**
 * @brief Polls a histogram's @c is_healthy() on an interval and calls back
 *        when it comes back false.
 *
 * @c target must outlive the monitor — the same address-stability contract
 * @c registry already places on the metrics it names. Construction starts
 * the background thread; destruction stops it and joins, so a monitor's
 * lifetime is exactly the span it watches for.
 *
 * Built on @c std::jthread rather than @c std::thread so stopping is a
 * request the polling loop can be woken for rather than a flag it has to be
 * taught to poll: @c condition_variable_any's stop_token overload registers
 * its own @c stop_callback that wakes the wait the instant @c request_stop()
 * runs, so @c stop_monitoring() needs no @c notify itself and no separate
 * "please stop" bit guarded by @c mutex_.
 *
 * @warning @p on_breach runs on the monitor's own thread, once per interval
 *          the histogram is unhealthy — not once per breach. A callback that
 *          blocks delays the next check by however long it runs.
 */
class sla_monitor {
public:
	using breach_callback = std::function<void(const histogram &)>;

	/// @param target Histogram to poll; must outlive this monitor.
	/// @param interval How often to poll. Small enough to catch a breach
	///        while it still matters, large enough not to matter on its own
	///        — @c metrics::settings::interval_ms is the existing knob this
	///        is meant to share with a caller that already exports on a
	///        timer.
	/// @param on_breach Called with @p target when a poll finds it
	///        unhealthy.
	CORE_EXPORT sla_monitor(const histogram &target,
							std::chrono::milliseconds interval,
							breach_callback on_breach);

	sla_monitor(const sla_monitor &)            = delete;
	sla_monitor &operator=(const sla_monitor &) = delete;
	sla_monitor(sla_monitor &&)                 = delete;
	sla_monitor &operator=(sla_monitor &&)      = delete;

	/// @brief Runs one check immediately, on the caller's thread, and calls
	///        @c on_breach if @c target is unhealthy right now.
	///
	/// The same check @c run() performs on a timer, exposed so a caller with
	/// its own reason to ask — e.g. the gap between the last periodic tick
	/// and the moment it stops watching — reuses the one callback instead of
	/// hand-rolling the same @c is_healthy()-then-warn a second time.
	CORE_EXPORT void check_now();

	/// @brief Stops the polling thread and joins it, ahead of destruction.
	///
	/// Idempotent — a caller that wants to stop watching before the monitor
	/// itself goes out of scope can call this directly, and the destructor
	/// calls it again unconditionally. @c jthread::request_stop() is already
	/// a no-op once a stop has been requested, and the second call finds
	/// @c worker_ no longer joinable, so neither call risks joining a thread
	/// twice.
	CORE_EXPORT void stop_monitoring();

	/// @brief @copybrief stop_monitoring
	CORE_EXPORT ~sla_monitor();

private:
	void run(const std::stop_token &token);

	const histogram &target_;
	std::chrono::milliseconds interval_;
	breach_callback on_breach_;

	std::mutex mutex_;
	std::condition_variable_any cv_;
	std::jthread worker_;
};

} // namespace exchange::core::metrics
