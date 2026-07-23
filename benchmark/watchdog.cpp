// Hard cap on total benchmark run time. A slow or runaway benchmark (the
// multi-threaded spin-wait ones can balloon) must not hang CI, so the process
// force-exits after a deadline — 2 minutes by default, override with the
// BENCH_MAX_SECONDS env var. Per-benchmark results already printed are kept;
// only the in-flight one is lost.
//
// Armed at static-init (before benchmark_main's main runs) via a detached
// daemon, so it needs no custom main and composes with benchmark_main.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

const int arm_watchdog = [] {
	int seconds = 120;
	if (const char *s = std::getenv("BENCH_MAX_SECONDS")) {
		if (const int v = std::atoi(s); v > 0) seconds = v;
	}
	std::thread([seconds] {
		std::this_thread::sleep_for(std::chrono::seconds(seconds));
		std::fprintf(stderr,
					 "\n[watchdog] benchmark run exceeded %ds — aborting\n",
					 seconds);
		std::_Exit(2);
	}).detach();
	return seconds;
}();

} // namespace
