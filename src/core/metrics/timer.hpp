#pragma once
// A minimal, portable per-batch latency timer for hot-path metrics.
//
// Deliberately not benchmark/latency.fixture.hpp's RDTSC cycle counter: that
// exists to resolve single-digit-nanosecond individual calls precisely
// enough for a benchmark's p99.9, and pays a one-time ~200 ms calibration
// against steady_clock to convert ticks to nanoseconds. Nothing here needs
// that precision - engine_partition times a whole drain *batch*, not one
// command, the same "batching is where the fixed cost goes" shape
// docs/performance.md already measures for risk_gate. That doc's own numbers
// for steady_clock::now() on Windows (~32 ns p50, ~73 ns p99, read once per
// batch) are inside budget once divided across a batch, so a plain
// steady_clock pair needs no platform asm and no startup stall.


#include "fwd.hpp"
#include "core_export.hpp"
#include <chrono>

namespace exchange::core::metrics {

/**
 * @brief RAII: records elapsed nanoseconds into a histogram on destruction.
 *
 * @code
 * {
 *     scoped_timer timer{metrics.drain_latency_ns};
 *     ... work being timed ...
 * } // recorded here
 * @endcode
 */
class scoped_timer {
public:
	CORE_EXPORT explicit scoped_timer(histogram &target) noexcept;

	scoped_timer(const scoped_timer &)            = delete;
	scoped_timer &operator=(const scoped_timer &) = delete;
	scoped_timer(scoped_timer &&)                 = delete;
	scoped_timer &operator=(scoped_timer &&)      = delete;

	CORE_EXPORT ~scoped_timer();

private:
	histogram *target_;
	std::chrono::steady_clock::time_point start_;
};

} // namespace exchange::core::metrics
