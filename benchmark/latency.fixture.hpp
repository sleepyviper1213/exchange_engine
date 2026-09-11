#pragma once
// Per-operation latency sampling, shared by any benchmark whose target is
// stated as a percentile rather than a mean.
//
// Google Benchmark cannot answer a p99 on its own: its aggregates are over
// *repetitions* of a whole timed loop, so one slow call disappears into the
// average of the millions around it. docs/performance.md asks for p50/p99/p99.9
// on every target, which means timing individual calls - and at ten nanoseconds
// a call that is right at the edge of what a clock can resolve. Hence the cycle
// counter, its calibration, and the overhead subtraction below.
//
// This lives at the top of benchmark/ rather than beside one component because
// two unrelated trees need it - order_book/ and risk/ - and copying a fixture
// between files is not an option. Same reasoning that puts cross-module
// benchmarks under app/.

#include "core/concurrency/affinity/affinity.hpp"
#include "core/metrics/quantile.hpp"
#include "core/util/function_ref.hpp"
#include "core/util/saturating.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define EXCHANGE_HAS_CYCLE_CLOCK 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#elif defined(__aarch64__)
#define EXCHANGE_HAS_CYCLE_CLOCK 1
#else
#define EXCHANGE_HAS_CYCLE_CLOCK 0
#endif

namespace exchange::bench {

namespace affinity = exchange::core::concurrency::affinity;

/**
 * @brief Pin this thread and raise its priority, via core's affinity layer.
 *
 * docs/performance.md: "Pin threads and state the topology. An unpinned run
 * measures the scheduler." Without this the tail is dominated by migrations and
 * preemptions rather than by the code - the order-book file's first run showed
 * maxima of 90 us to 1 ms against a p50 of 30 ns, which is the OS, not the
 * book. It does not make the machine quiet; it removes the two sources of tail
 * noise a benchmark can remove by itself.
 *
 * Both calls are best-effort by contract, and the priority one needs an
 * elevated process on Windows. The result is published in the @c pinned counter
 * so a run that silently failed to pin is not read as a clean one.
 *
 * @return true only if both the pin and the priority took effect.
 */
[[nodiscard]] inline bool pin_and_prioritise(affinity::core_id core) {
	const bool pinned = affinity::pin_this_thread(core);
	const bool raised =
		affinity::set_this_thread_priority(affinity::thread_priority::high);
	return pinned && raised;
}

#if EXCHANGE_HAS_CYCLE_CLOCK

/**
 * @brief Open a timed region: fence, then read the counter.
 *
 * @c lfence first so the read cannot drift above whatever preceded it; plain
 * @c rdtsc after, because the fence has already done the ordering @c rdtscp
 * would repeat. Pairing this with @c cycle_stop rather than using a symmetric
 * serialising read at both ends is what keeps the floor near 5 ns instead of 15
 * - at a p50 under 30 ns the clock is a third of the result, so the asymmetry
 * is worth the extra function.
 */
[[nodiscard]] inline std::uint64_t cycle_start() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	_mm_lfence();
	return __rdtsc();
#else
	std::uint64_t stamp = 0;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(stamp)::"memory");
	return stamp;
#endif
}

/**
 * @brief Close a timed region.
 *
 * @c rdtscp does not retire until every prior instruction has, so it captures
 * the work under test without needing a fence in front of it.
 */
[[nodiscard]] inline std::uint64_t cycle_stop() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	unsigned aux = 0;
	return __rdtscp(&aux);
#else
	std::uint64_t stamp = 0;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(stamp)::"memory");
	return stamp;
#endif
}

/**
 * @brief Cycle-counter ticks per nanosecond, measured against steady_clock.
 *
 * The x86 TSC is invariant - it counts at a fixed rate regardless of the core's
 * current frequency - so this converts ticks to wall-clock nanoseconds, not to
 * core cycles. A number here is what a user waits, which is the thing a 50 ns
 * budget is denominated in.
 */
[[nodiscard]] inline double ticks_per_ns() {
	using clock           = std::chrono::steady_clock;
	const auto wall_start = clock::now();
	const auto tick_start = cycle_start();

	// Long enough that steady_clock's own resolution is noise, short enough not
	// to stall the suite.
	while (clock::now() - wall_start < std::chrono::milliseconds(200))
		benchmark::DoNotOptimize(cycle_stop());

	const auto tick_end = cycle_stop();
	const auto wall_end = clock::now();
	const auto elapsed_ns =
		std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end -
															 wall_start)
			.count();
	return static_cast<double>(tick_end - tick_start) /
		   static_cast<double>(elapsed_ns);
}

/**
 * @brief The cost of the measurement itself, in ticks.
 *
 * Two back-to-back reads with nothing between them. Subtracting the median of
 * this from every sample removes the harness from the result; it cannot remove
 * the *variance* the harness adds, which is why p99.9 is reported but not
 * trusted as tightly as p50 and p99.
 */
[[nodiscard]] inline std::uint64_t clock_overhead_ticks() {
	std::vector<std::uint64_t> samples;
	samples.reserve(20000);
	for (int i = 0; i < 20000; ++i) {
		const auto start = cycle_start();
		const auto stop  = cycle_stop();
		samples.push_back(stop - start);
	}
	std::ranges::sort(samples);
	return samples[samples.size() / 2];
}

/**
 * @brief Times individual operations and publishes their distribution.
 *
 * @par Usage
 * @code
 * latency_sampler sampler;
 * for (auto _ : state) {
 *     sampler.sample([&] { benchmark::DoNotOptimize(gate.submit(cmd)); });
 *     restore_state();  // untimed, but still on Google Benchmark's clock
 * }
 * sampler.publish(state);
 * @endcode
 *
 * @note Because untimed work stays inside the loop, the reported @c Time column
 *       is a per-iteration average of *everything*, not a per-sample one. The
 *       percentile counters are the answer; read @c Time only as a sanity
 * check.
 *
 * Calibration is per process, not per benchmark - the tick rate and the clock
 * overhead do not change between families, and re-measuring them would add
 * 200 ms to every registered benchmark.
 */
class latency_sampler {
public:
	/// @brief Default cap on retained samples. A million is enough to place a
	///        p99.9 with three digits behind it and costs 8 MB.
	static constexpr std::size_t DEFAULT_CAPACITY = 1U << 20U;

	/// @param core Which core to pin to. Pick one the rest of the suite is not
	///        using.
	/// @param capacity Samples to retain; the rest are timed and dropped.
	explicit latency_sampler(affinity::core_id core = 2,
							 std::size_t capacity   = DEFAULT_CAPACITY) {
		static const bool pinned_once   = pin_and_prioritise(core);
		static const double rate        = ticks_per_ns();
		static const std::uint64_t cost = clock_overhead_ticks();
		pinned_                         = pinned_once;
		per_ns_                         = rate;
		overhead_                       = cost;
		samples_.reserve(capacity);
	}

	/// @brief Time one invocation of @p operation and retain the sample.
	void sample(core::util::function_ref<void() const> operation) {
		const auto start = cycle_start();
		operation();
		const auto stop = cycle_stop();
		if (samples_.size() < samples_.capacity())
			samples_.push_back(stop - start);
	}

	/// @brief p50 / p99 / p99.9 / max in nanoseconds, harness overhead removed,
	///        into @p state's counters.
	void publish(benchmark::State &state) {
		using namespace exchange::core::metrics;

		if (samples_.empty()) return;
		std::ranges::sort(samples_);

		// The rank comes from `core::metrics::rank_of`, which is what the
		// engine's own histogram uses. It did not: this took rank
		// `floor(q * (n - 1))` where the histogram takes `floor(q * n)`, so at
		// n=100 a p99 here was the 99th sample and a p99 there was the 100th.
		// A benchmark figure that cannot be reproduced by the running engine is
		// worse than no figure, and the difference was invisible at both call
		// sites. @see core/metrics/quantile.hpp
		const auto quantile = [&](percentile q) {
			const auto ticks = samples_[rank_of(q, samples_.size())];
			// Saturate rather than wrap: a sample can land below the median
			// overhead purely by measurement jitter.
			const auto net = core::util::saturating_sub(ticks, overhead_);
			return static_cast<double>(net) / per_ns_;
		};

		state.counters["p50_ns"]  = quantile(percentile::P50);
		state.counters["p99_ns"]  = quantile(percentile::P99);
		state.counters["p999_ns"] = quantile(percentile::P999);
		state.counters["max_ns"]  = quantile(percentile::PMAX);
		state.counters["samples"] = static_cast<double>(samples_.size());
		// The noise floor, published so the columns above can be read against
		// it. A p50 within a few ns of this is measuring the clock.
		state.counters["clock_ns"] = static_cast<double>(overhead_) / per_ns_;
		state.counters["pinned"]   = pinned_ ? 1 : 0;
	}

private:
	std::vector<std::uint64_t> samples_;
	double per_ns_          = 1.0;
	std::uint64_t overhead_ = 0;
	bool pinned_            = false;
};

#endif // EXCHANGE_HAS_CYCLE_CLOCK

} // namespace exchange::bench
