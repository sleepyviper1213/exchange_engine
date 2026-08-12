// Cost of the primitives docs/performance.md's "Production systems" section
// is measured against: a counter bump and a histogram record, each meant to
// sit on a path budgeted in nanoseconds. Mean throughput first (this file),
// then the distribution — a single slow call can matter more than the mean
// on a path that runs once per drained batch, the same reasoning
// trading-engine/risk/latency.bench.cpp gives for the gate.

#include "latency.fixture.hpp"

#include "core/metrics/counter.hpp"
#include "core/metrics/histogram.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

using exchange::bench::latency_sampler;
using exchange::core::metrics::counter;
using exchange::core::metrics::histogram;

namespace {

void BM_MetricsCounter_Add(benchmark::State &state) {
	counter c;
	std::uint64_t i = 0;
	for (auto _ : state) {
		c.add(1);
		benchmark::ClobberMemory();
		++i;
	}
	benchmark::DoNotOptimize(c.load());
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_MetricsCounter_Add);

void BM_MetricsHistogram_Record(benchmark::State &state) {
	histogram h;
	std::uint64_t value = 1;
	for (auto _ : state) {
		h.record(value);
		benchmark::ClobberMemory();
		// Walk through every bucket across iterations rather than hammering
		// one, since a production caller's values are not degenerate either.
		value = (value << 1U) | 1U;
		if (value == 0) value = 1;
	}
	benchmark::DoNotOptimize(h.read().total);
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_MetricsHistogram_Record);

#if EXCHANGE_HAS_CYCLE_CLOCK

void BM_MetricsLatency_CounterAdd(benchmark::State &state) {
	counter c;
	latency_sampler sampler;
	for (auto _ : state)
		sampler.sample([&] {
			c.add(1);
			benchmark::ClobberMemory();
		});
	sampler.publish(state);
}

BENCHMARK(BM_MetricsLatency_CounterAdd);

void BM_MetricsLatency_HistogramRecord(benchmark::State &state) {
	histogram h;
	latency_sampler sampler;
	std::uint64_t value = 1;
	for (auto _ : state) {
		sampler.sample([&] {
			h.record(value);
			benchmark::ClobberMemory();
		});
		value = (value << 1U) | 1U;
		if (value == 0) value = 1;
	}
	sampler.publish(state);
}

BENCHMARK(BM_MetricsLatency_HistogramRecord);

#endif // EXCHANGE_HAS_CYCLE_CLOCK

} // namespace
