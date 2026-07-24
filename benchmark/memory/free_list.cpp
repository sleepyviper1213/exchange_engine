#include "memory/free_list.hpp"
#include "memory/free_list_hazard.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <vector>

// Head-to-head for the arena's two FreeList reclamation strategies: the tagged
// (versioned-pointer) default vs the hazard-pointer alternative. The interfaces
// are identical, so one templated body benchmarks both. The question these
// answer is which strategy to keep inline: the single-thread floor, and how each
// degrades when many threads share one list and contend on the head.
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

BENCHMARK_TEMPLATE(BM_FreeList_ST, memory::tagged::free_list)->Name(
	"BM_FreeList_ST_Tagged");
BENCHMARK_TEMPLATE(BM_FreeList_ST, memory::hazard::free_list)->Name(
	"BM_FreeList_ST_Hazard");

// --- Shared list under contention: all threads pop/push the same list. -------

template <class FreeList>
void BM_FreeList_MT(benchmark::State &state) {
	static std::unique_ptr<std::vector<Block> > pool;
	static std::unique_ptr<FreeList> fl;
	// Thread 0 owns the shared state; the timed loop's start barrier guarantees
	// it is built before any thread enters.
	if (state.thread_index() == 0) {
		pool = std::make_unique<std::vector<Block> >(kBlocks);
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

BENCHMARK_TEMPLATE(BM_FreeList_MT, memory::tagged::free_list)
	->Name("BM_FreeList_MT_Tagged")
	->ThreadRange(1, 16)
	->UseRealTime();
BENCHMARK_TEMPLATE(BM_FreeList_MT, memory::hazard::free_list)
	->Name("BM_FreeList_MT_Hazard")
	->ThreadRange(1, 16)
	->UseRealTime();
} // namespace