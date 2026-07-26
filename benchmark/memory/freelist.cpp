#include "core/memory/detail/freelist.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Micro-benchmarks for memory::pool::freelist — the hazard-pointer,
// ABA-safe object pool. Unlike memory::object_pool (one pool per book side, no
// sharing), this pool is meant to be *shared* across threads, so the number
// that justifies it is the contended multi-producer/consumer case: many threads
// acquiring and releasing from one pool. The single-thread cost and new/delete
// are measured alongside as the floor and the reference to beat.
namespace {

using exchange::core::memory::pool::free_list;
using namespace exchange::core::memory;

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
void BM_PoolFreeList_ST_AcquireRelease(benchmark::State &state) {
	free_list<PooledOrder> pool(kPoolCapacity);

	for (auto _ : state) {
		PooledOrder *obj = pool.acquire();
		benchmark::DoNotOptimize(obj);
		obj->id = 1; // touch the node so the round-trip is not elided
		benchmark::DoNotOptimize(obj->id);
		pool.release(obj);
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PoolFreeList_ST_AcquireRelease);

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

// One pool shared by all benchmark threads, each hammering acquire/release.
// This is what memory::object_pool cannot do safely and what the hazard-pointer
// recycling buys. Thread 0 owns the shared pool's lifetime; the timed loop's
// start barrier guarantees it is constructed before any thread enters. Watch
// how per-thread throughput degrades as threads contend on free_head_ and the
// domain.
void BM_PoolFreeList_MT_Contended(benchmark::State &state) {
	static std::unique_ptr<free_list<PooledOrder>> shared;
	if (state.thread_index() == 0)
		shared = std::make_unique<free_list<PooledOrder>>(kPoolCapacity);

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

BENCHMARK(BM_PoolFreeList_MT_Contended)
->ThreadRange(1, 16)->UseRealTime();

} // namespace

// Head-to-head for the arena's two FreeList reclamation strategies: the tagged
// (versioned-pointer) default vs the hazard-pointer alternative. The interfaces
// are identical, so one templated body benchmarks both. The question these
// answer is which strategy to keep inline: the single-thread floor, and how
// each degrades when many threads share one list and contend on the head.
namespace {
// Cache-aligned, cache-line-sized blocks — above either FreeList's
// kMinBlockBytes, and one block per line so contention is on the list head, not
// on false-shared payloads.
struct alignas(64) Block {
	std::byte bytes[64];
};

// Large relative to the hazard version's reclamation threshold, so a steady
// ping-pong never drains the free stack and stalls on a sweep.
inline constexpr std::size_t kBlocks = 4096;

// --- Single thread: pop immediately followed by push, one block in flight. ---

template <class FreeList>
void BM_FreeList_ST(benchmark::State &state) {
	std::vector<Block> pool(kBlocks);
	FreeList fl;
	for (auto &blk : pool) fl.push(&blk);

	for (auto _ : state) {
		void *b = fl.pop();
		benchmark::DoNotOptimize(b);
		fl.push(b);
	}
	state.SetItemsProcessed(state.iterations());

	while (fl.pop() != nullptr) {} // drain before the pool goes away
}
BENCHMARK_TEMPLATE(BM_FreeList_ST, tagged::free_list)
	->Name("BM_FreeList_ST_Tagged");
BENCHMARK_TEMPLATE(BM_FreeList_ST, hazard::free_list)
	->Name("BM_FreeList_ST_Hazard");

// --- Shared list under contention: all threads pop/push the same list. -------

template <class FreeList>
void BM_FreeList_MT(benchmark::State &state) {
	static std::unique_ptr<std::vector<Block>> pool;
	static std::unique_ptr<FreeList> fl;
	// Thread 0 owns the shared state; the timed loop's start barrier guarantees
	// it is built before any thread enters.
	if (state.thread_index() == 0) {
		pool = std::make_unique<std::vector<Block>>(kBlocks);
		fl   = std::make_unique<FreeList>();
		for (auto &blk : *pool) fl->push(&blk);
	}

	for (auto _ : state) {
		void *b = fl->pop();
		if (b == nullptr) continue; // momentarily drained; retry
		benchmark::DoNotOptimize(b);
		fl->push(b);
	}
	state.SetItemsProcessed(state.iterations());

	if (state.thread_index() == 0) {
		while (fl->pop() != nullptr) {}
		fl.reset();
		pool.reset();
	}
}

BENCHMARK_TEMPLATE(BM_FreeList_MT, tagged::free_list)
	->Name("BM_FreeList_MT_Tagged")
	->ThreadRange(1, 16)
	->UseRealTime();
BENCHMARK_TEMPLATE(BM_FreeList_MT, hazard::free_list)
	->Name("BM_FreeList_MT_Hazard")
	->ThreadRange(1, 16)
	->UseRealTime();
} // namespace
