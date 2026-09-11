#include "core/metrics/sla_monitor.hpp"

#include "core/metrics/histogram.hpp"

#include <gtest/gtest.h>
#include <spdlog/stopwatch.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

using exchange::core::metrics::histogram;
using exchange::core::metrics::latency_budgets;
using exchange::core::metrics::sla_monitor;
using namespace std::chrono_literals;

namespace {

// Lets a test block until the callback has fired at least once, instead of
// polling - the monitor's own condition_variable makes this the natural
// shape for the test side too.
class breach_latch {
public:
	void notify() {
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			++count_;
		}
		cv_.notify_one();
	}

	bool wait_for_first(std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mutex_);
		return cv_.wait_for(lock, timeout, [&] { return count_ > 0; });
	}

	int count() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		return count_;
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	int count_ = 0;
};

TEST(MetricsSlaMonitor, CallsBackWhenTheHistogramIsUnhealthy) {
	histogram h{latency_budgets{.p99 = std::chrono::nanoseconds{1}}};
	h.record(1'000'000); // far past the 1 ns budget

	breach_latch latch;
	const sla_monitor monitor(h, 10ms, [&](const histogram &) {
		latch.notify();
	});

	EXPECT_TRUE(latch.wait_for_first(1s));
}

TEST(MetricsSlaMonitor, NeverCallsBackWhileTheHistogramStaysHealthy) {
	histogram h{latency_budgets{.p99 = 1ms}};
	h.record(1); // well inside budget

	breach_latch latch;
	const sla_monitor monitor(h, 10ms, [&](const histogram &) {
		latch.notify();
	});

	EXPECT_FALSE(latch.wait_for_first(100ms));
}

TEST(MetricsSlaMonitor, CheckNowRunsSynchronouslyWithoutWaitingForATick) {
	histogram h{latency_budgets{.p99 = std::chrono::nanoseconds{1}}};
	h.record(1'000'000); // far past the 1 ns budget

	breach_latch latch;
	// An interval longer than the test itself: any callback observed has to
	// have come from check_now(), not from run()'s own timer.
	sla_monitor monitor(h, 10s, [&](const histogram &) { latch.notify(); });

	monitor.check_now();

	EXPECT_TRUE(latch.wait_for_first(100ms));
}

TEST(MetricsSlaMonitor, CheckNowDoesNothingWhileHealthy) {
	histogram h{latency_budgets{.p99 = 1ms}};
	h.record(1); // well inside budget

	breach_latch latch;
	sla_monitor monitor(h, 10s, [&](const histogram &) { latch.notify(); });

	monitor.check_now();

	EXPECT_FALSE(latch.wait_for_first(100ms));
}

TEST(MetricsSlaMonitor,
	 DestructionStopsPromptlyRatherThanWaitingOutTheInterval) {
	const histogram h; // never recorded into - always healthy
	std::optional<sla_monitor> monitor;
	monitor.emplace(h, 10s, [](const histogram &) {});

	const spdlog::stopwatch stopwatch;
	monitor.reset();

	EXPECT_LT(stopwatch.elapsed(), 1s);
}

TEST(MetricsSlaMonitor,
	 StopMonitoringStopsPromptlyRatherThanWaitingOutTheInterval) {
	const histogram h; // never recorded into - always healthy
	sla_monitor monitor(h, 10s, [](const histogram &) {});

	const spdlog::stopwatch stopwatch;
	monitor.stop_monitoring();

	EXPECT_LT(stopwatch.elapsed(), 1s);
}

TEST(MetricsSlaMonitor, StopMonitoringSilencesFurtherCallbacks) {
	histogram h{latency_budgets{.p99 = std::chrono::nanoseconds{1}}};
	h.record(1'000'000); // far past the 1 ns budget

	breach_latch latch;
	sla_monitor monitor(h, 10ms, [&](const histogram &) { latch.notify(); });

	ASSERT_TRUE(latch.wait_for_first(1s));
	monitor.stop_monitoring();
	const int count_at_stop = latch.count();

	// Long enough that a still-running timer would have ticked several
	// times over; the count must not have moved since stop_monitoring()
	// returned.
	std::this_thread::sleep_for(100ms);
	EXPECT_EQ(latch.count(), count_at_stop);
}

TEST(MetricsSlaMonitor, StopMonitoringIsIdempotent) {
	const histogram h; // never recorded into - always healthy
	sla_monitor monitor(h, 10ms, [](const histogram &) {});

	monitor.stop_monitoring();
	// A second explicit call, then the destructor's own call: neither may
	// join an already-joined thread.
	monitor.stop_monitoring();
}

} // namespace
