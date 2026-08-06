// Usage of concurrency::affinity::topology's LLC map. Measures SPSC round-trip
// latency with the two ends pinned to cores that SHARE a last-level cache vs
// cores on SEPARATE LLCs (different socket / CCX). The gap is the cost the
// matching engine's command queue pays when producer and consumer land far
// apart. On a single-LLC host the "separate" case doesn't exist and simply
// isn't registered — the placement comes straight from topology, no hand-picked
// core numbers.

#include "core/concurrency/affinity.hpp" // discover, topology, pin_this_thread
#include "core/concurrency/lockfree/spsc_queue.hpp" // concurrency::spsc_queue

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace {
using namespace exchange::core::concurrency;
using lockfree::spsc_queue;
using Pair = std::pair<affinity::core_id, affinity::core_id>;

// First distinct primary-core pair whose LLC-sharing matches @p share. One
// primary sibling per physical core keeps SMT-sibling effects out of the
// signal.
std::optional<Pair> pick_pair(const affinity::topology &topo, bool share) {
	const auto cores = topo.primary_core_ids();
	for (std::size_t i = 0; i < cores.size(); ++i)
		for (std::size_t j = i + 1; j < cores.size(); ++j)
			if (topo.share_llc(cores[i], cores[j]) == share)
				return Pair{cores[i], cores[j]};
	return std::nullopt;
}

// One token round-trips a -> b -> a through two SPSC queues; each iteration is
// a single ping-pong, so the reported time is the cross-core hand-off latency.
void BM_PingPong(benchmark::State &state, const Pair &pair) {
	spsc_queue<std::uint64_t, 1024> to_b;
	spsc_queue<std::uint64_t, 1024> to_a;
	std::atomic<bool> stop{false};

	const auto &[a, b] = pair;
	// Echo end on core b: pop from to_b, push back into to_a.
	std::thread echo([&] {
		static_cast<void>(affinity::pin_this_thread(b));
		std::uint64_t v = 0;
		while (!stop.load(std::memory_order_acquire))
			if (to_b.try_dequeue(v))
				while (!to_a.try_emplace(v))
					if (stop.load(std::memory_order_acquire)) return;
	});

	// Ping end on core a: this thread.
	static_cast<void>(affinity::pin_this_thread(a));
	std::uint64_t token  = 0;
	std::uint64_t echoed = 0;
	for (auto _ : state) {
		while (!to_b.try_emplace(token)) {}
		while (!to_a.try_dequeue(echoed)) {}
		++token;
	}
	benchmark::DoNotOptimize(echoed);

	stop.store(true, std::memory_order_release);
	static_cast<void>(to_b.try_emplace(0)); // unblock a waiting echo, then join
	echo.join();

	state.SetItemsProcessed(state.iterations());
}

// Register only the placements the host actually offers — the topology decides.
const int registrar = [] {
	const affinity::topology topo = affinity::discover();
	if (const auto shared = pick_pair(topo, /*share=*/true))
		RegisterBenchmark("SPSC_pingpong/LLC_shared",
									 BM_PingPong,
									 *shared)
			->UseRealTime();
	if (const auto separate = pick_pair(topo, /*share=*/false))
		RegisterBenchmark("SPSC_pingpong/LLC_separate",
									 BM_PingPong,
									 *separate)
			->UseRealTime();
	return 0;
}();
} // namespace
