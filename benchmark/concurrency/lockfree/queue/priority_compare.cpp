// Studies the effect of affinity::ThreadPriority on a pinned SPSC hand-off:
// the same producer->consumer stream run at Normal vs High priority. It exists
// because raising priority helps latency under *contention* but tends to hurt
// an isolated spin-wait throughput benchmark — boosting two busy-waiters that
// have no competitor only starves the OS/harness helpers and worsens overlap.
// Here that trade-off is the measured quantity, not an accident (bench_cores()
// stays at Normal so it isn't).
//
// Both ends are spawned worker threads pinned via topology and set to the tier
// under test, then destroyed each timed run — so nothing pins or re-prioritizes
// google-benchmark's own thread. Manual timing covers just the N-item transfer.

#include "core/concurrency.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace exchange::core::concurrency;
using lockfree::spsc_queue;
using Pair = std::pair<affinity::CoreId, affinity::CoreId>;

inline constexpr std::size_t kCapacity = 1UL << 14; // power of two
inline constexpr std::uint64_t kItems  = 1UL << 20; // items moved per timed run

// Stream kItems from a producer on core `pair.first` to a consumer on
// `pair.second`, both pinned and set to `prio`, and time just the transfer.
void BM_Stream(benchmark::State &state, Pair pair,
			   affinity::ThreadPriority prio) {
	const auto [prod_core, cons_core] = pair;

	for (auto _ : state) {
		spsc_queue<std::uint64_t, kCapacity> q;
		std::atomic<bool> go{false};

		std::thread producer([&] {
			static_cast<void>(affinity::pin_this_thread(prod_core));
			static_cast<void>(affinity::set_this_thread_priority(prio));
			while (!go.load(std::memory_order_acquire)) {}
			for (std::uint64_t v = 0; v < kItems; ++v)
				while (!q.try_emplace(v)) {}
		});
		std::thread consumer([&] {
			static_cast<void>(affinity::pin_this_thread(cons_core));
			static_cast<void>(affinity::set_this_thread_priority(prio));
			while (!go.load(std::memory_order_acquire)) {}
			std::uint64_t out = 0;
			for (std::uint64_t n = 0; n < kItems;)
				if (q.try_dequeue(out)) ++n;
			benchmark::DoNotOptimize(out);
		});

		const auto start = std::chrono::steady_clock::now();
		go.store(true, std::memory_order_release);
		producer.join();
		consumer.join();
		const auto elapsed = std::chrono::steady_clock::now() - start;

		state.SetIterationTime(std::chrono::duration<double>(elapsed).count());
	}
	state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
							static_cast<std::int64_t>(kItems));
}

const int registrar = [] {
	const affinity::Topology topo = affinity::discover();
	const auto cores              = topo.primary_core_ids();
	if (cores.size() < 2) return 0; // need two distinct physical cores
	const Pair pair{cores[0], cores[1]};

	for (const auto &[name, prio] :
		 {std::pair{"Normal", affinity::ThreadPriority::Normal},
		  std::pair{"High", affinity::ThreadPriority::High}})
		benchmark::RegisterBenchmark(std::string("SPSC_stream/prio_") + name,
									 BM_Stream,
									 pair,
									 prio)
			->UseManualTime();
	return 0;
}();
} // namespace
