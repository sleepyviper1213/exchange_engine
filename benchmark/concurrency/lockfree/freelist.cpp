#include "concurrency/lockfree/freelist.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <vector>

// Micro-benchmarks for concurrency::lockfree::freelist — the hazard-pointer,
// ABA-safe object pool. Unlike memory::object_pool (one pool per book side, no
// sharing), this pool is meant to be *shared* across threads, so the number that
// justifies it is the contended multi-producer/consumer case: many threads
// acquiring and releasing from one pool. The single-thread cost and new/delete
// are measured alongside as the floor and the reference to beat.
namespace {

using concurrency::lockfree::freelist;

// A representative resting-order node: a few 8-byte fields, ~40 bytes, so the
// measurement reflects moving a real node-sized object rather than an int.
struct PooledOrder {
	std::uint64_t id;
	std::uint64_t price;
	std::int64_t volume;
	std::uint32_t flags;
	std::uint32_t sequence;
};

// Sized well above the working set so acquire() is always served from the free
// stack and never reports exhaustion. Large relative to the reclamation
// threshold, so released nodes always drain back before the pool runs dry.
inline constexpr std::size_t kPoolCapacity = 1U << 16U;

// --- Single thread -----------------------------------------------------------

// Steady-state ping-pong: acquire immediately followed by release, one object
// live at a time. Isolates the per-op cost of the hazard-pointer pop plus the
// retire/recycle round-trip with no cross-core coherency traffic — the floor.
void BM_Freelist_ST_AcquireRelease(benchmark::State &state) {
	freelist<PooledOrder> pool(kPoolCapacity);

	for (auto _ : state) {
		PooledOrder *obj = pool.acquire();
		benchmark::DoNotOptimize(obj);
		obj->id = 1; // touch the node so the round-trip is not elided
		benchmark::DoNotOptimize(obj->id);
		pool.release(obj);
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_Freelist_ST_AcquireRelease);

// Reference: the same ping-pong against the global allocator. The pool's ST
// number is only meaningful relative to this.
void BM_NewDelete_ST_AllocFree(benchmark::State &state) {
	for (auto _ : state) {
		auto *obj = new PooledOrder();
		benchmark::DoNotOptimize(obj);
		obj->id = 1;
		benchmark::DoNotOptimize(obj->id);
		delete obj;
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_NewDelete_ST_AllocFree);

// --- Shared pool under contention (the reason this type exists) --------------

// One pool shared by all benchmark threads, each hammering acquire/release. This
// is what memory::object_pool cannot do safely and what the hazard-pointer
// recycling buys. Thread 0 owns the shared pool's lifetime; the timed loop's
// start barrier guarantees it is constructed before any thread enters. Watch how
// per-thread throughput degrades as threads contend on free_head_ and the domain.
void BM_Freelist_Shared_Contended(benchmark::State &state) {
	static std::unique_ptr<freelist<PooledOrder>> shared;
	if (state.thread_index() == 0)
		shared = std::make_unique<freelist<PooledOrder>>(kPoolCapacity);

	for (auto _ : state) {
		PooledOrder *obj = shared->acquire();
		if (obj == nullptr) continue; // momentarily drained; retry
		benchmark::DoNotOptimize(obj);
		obj->id = 1;
		benchmark::DoNotOptimize(obj->id);
		shared->release(obj);
	}
	state.SetItemsProcessed(state.iterations());

	if (state.thread_index() == 0) shared.reset();
}

BENCHMARK(BM_Freelist_Shared_Contended)->ThreadRange(1, 16)->UseRealTime();

} // namespace
