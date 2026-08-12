// What core/metrics costs an engine_partition that opts into it: the
// per-drain overhead of one steady_clock pair plus three counter bumps and a
// histogram record, against a partition that carries no metrics at all.
// docs/performance.md's order-processing budget is a latency target, not a
// throughput one, so this reports p50/p99/p999 through latency.fixture.hpp
// rather than a mean.

#include "latency.fixture.hpp"

#include "trading-engine/event/command.hpp"
#include "trading-engine/execution/engine_partition.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange;
using exchange::bench::latency_sampler;

namespace {

using Engine = engine_partition<1U << 12>;

// A small, fixed-size crossing batch: the unit engine_partition's own
// drain_latency_ns histogram times is one drain() call, so each sampled
// operation here is "submit this batch, then drain it" — not one command.
constexpr std::size_t BATCH = 64;

std::vector<command> make_crossing_batch(std::size_t n, price_t base) {
	std::vector<command> cmds;
	cmds.reserve(n);
	for (std::size_t i = 0; i < n; i += 2) {
		const price_t price      = base + static_cast<price_t>(i / 2);
		constexpr quantity_t qty = 10;
		cmds.push_back(command::place(order{.id    = i + 1,
											.side  = side_t::ask,
											.price = price,
											.qty   = qty}));
		cmds.push_back(command::place(order{.id    = i + 2,
											.side  = side_t::bid,
											.price = price,
											.qty   = qty}));
	}
	return cmds;
}

#ifdef EXCHANGE_HAS_CYCLE_CLOCK

void BM_EnginePartitionLatency_DrainNoMetrics(benchmark::State &state) {
	auto engine = std::make_unique<Engine>(nullptr);
	engine->listing(0);

	latency_sampler sampler;
	price_t base = 1;
	for (auto _ : state) {
		const auto batch = make_crossing_batch(BATCH, base);
		base += static_cast<price_t>(BATCH);
		for (const command &cmd : batch) engine->submit(cmd);
		sampler.sample([&] { benchmark::DoNotOptimize(engine->drain()); });
		engine->flush();
	}
	sampler.publish(state);
}

BENCHMARK(BM_EnginePartitionLatency_DrainNoMetrics);

// Also checks the production histogram against the ground truth: every
// drain() timed by the sampler below is *also* timed by
// partition_metrics::drain_latency_ns through the real scoped_timer in
// engine_partition::drain(), so by the end of the loop that histogram holds
// its own p50/p99/p999 for the exact same calls. Publishing both lets a
// reader see how close the coarse, bucketed, always-on histogram
// (histogram.hpp trades precision for O(1) bounded memory — see its header
// comment) lands next to latency_sampler's exact-sample p50/p99/p999,
// rather than trusting the bucketing argument on paper.
void BM_EnginePartitionLatency_DrainWithMetrics(benchmark::State &state) {
	partition_metrics metrics;
	auto engine = std::make_unique<Engine>(nullptr,
										   Engine::OutcomeSink{},
										   book_manager::DEFAULT_BOOK_CAPACITY,
										   order_manager::DEFAULT_CAPACITY,
										   &metrics);
	engine->listing(0);

	latency_sampler sampler;
	price_t base = 1;
	for (auto _ : state) {
		const auto batch = make_crossing_batch(BATCH, base);
		base += static_cast<price_t>(BATCH);
		for (const command &cmd : batch) engine->submit(cmd);
		sampler.sample([&] { benchmark::DoNotOptimize(engine->drain()); });
		engine->flush();
	}
	sampler.publish(state);

	// The histogram accumulates across every iteration Google Benchmark ran
	// (it auto-tunes the iteration count), so its snapshot is read once,
	// after the loop, over strictly more samples than any single iteration.
	const auto snapshot = metrics.drain_latency_ns.read();
	state.counters["snapshot_p50_ns"] =
		static_cast<double>(snapshot.quantile(0.50));
	state.counters["snapshot_p99_ns"] =
		static_cast<double>(snapshot.quantile(0.99));
	state.counters["snapshot_p999_ns"] =
		static_cast<double>(snapshot.quantile(0.999));
	state.counters["snapshot_n"] = static_cast<double>(snapshot.total);
}

BENCHMARK(BM_EnginePartitionLatency_DrainWithMetrics);

#endif // EXCHANGE_HAS_CYCLE_CLOCK

} // namespace
