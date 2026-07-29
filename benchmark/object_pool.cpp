#include "core/memory/object_pool.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

// Micro-benchmarks for memory::object_pool — the lock-free MPMC ring
// pool that hands out pre-constructed nodes (allocate() -> T*, free(T*)) and
// falls back to new/delete only when drained. The order book allocates and frees
// a resting-order node on essentially every message, so the two numbers that
// matter are (1) the steady-state per-op cost on a single core and (2) how that
// cost degrades when several book sides share one pool and contend on the ring
// cursors. new/delete is benchmarked alongside as the reference the pool has to
// beat to justify existing.
namespace {

using exchange::core::memory::object_pool;

// A representative resting-order node: a handful of 8-byte fields, ~40 bytes, so
// the measurement reflects moving a real node-sized object rather than an int.
struct pooled_order {
    std::uint64_t id;
    std::uint64_t price;
    std::int64_t  qty;
    std::uint32_t flags;
    std::uint32_t sequence;
};

// Sized well above any working set the single-thread cases touch, so allocate()
// is always served from the ring and never trips the heap fallback (which would
// measure the allocator underneath instead of the pool). Power of two, as the
// pool's index masking requires.
inline constexpr std::uint32_t kPoolCapacity = 1U << 16U;

// --- Single thread -----------------------------------------------------------

// Steady-state ping-pong: allocate immediately followed by free, one object live
// at a time. Keeps a single ring slot hot in L1 and isolates the per-operation
// instruction cost of consume() + reserve()/publish() with zero cross-core
// coherency traffic. This is the lower bound — the best the pool can ever do.
void BM_ObjectPool_ST_AllocFree(benchmark::State &state) {
    object_pool<pooled_order> pool(kPoolCapacity);

    for (auto _ : state) {
        pooled_order *obj = pool.allocate();
        benchmark::DoNotOptimize(obj);
        obj->id = 1; // touch the node so the round-trip cannot be optimized away
        benchmark::DoNotOptimize(obj->id);
        pool.free(obj);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ObjectPool_ST_AllocFree);

// Reference: the same ping-pong against the global allocator. The pool's ST
// number is only meaningful relative to this — it is the baseline the ring buffer
// exists to replace on the hot path.
void BM_NewDelete_ST_AllocFree(benchmark::State &state) {
    for (auto _ : state) {
        auto *obj = new pooled_order();
        benchmark::DoNotOptimize(obj);
        obj->id = 1;
        benchmark::DoNotOptimize(obj->id);
        delete obj;
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_NewDelete_ST_AllocFree);

// Bulk churn: drain N nodes out of the pool, then return all N, per iteration.
// Unlike the ping-pong this walks the ring across many slots (touching more of
// the control array and objects), so it captures the cost once the working set
// spills L1/L2 and exercises the wrap logic. N is capped at capacity, so every
// allocation still comes from the pool (no heap fallback). The pointer vector is
// hoisted out of the timed loop so only allocate/free are measured.
void BM_ObjectPool_ST_BulkChurn(benchmark::State &state) {
    const auto n = static_cast<std::uint32_t>(state.range(0));
    object_pool<pooled_order> pool(kPoolCapacity);
    std::vector<pooled_order *> held(n, nullptr);

    for (auto _ : state) {
        for (std::uint32_t i = 0; i < n; ++i) {
            held[i] = pool.allocate();
            benchmark::DoNotOptimize(held[i]);
        }
        for (std::uint32_t i = 0; i < n; ++i) pool.free(held[i]);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

BENCHMARK(BM_ObjectPool_ST_BulkChurn)
    ->RangeMultiplier(8)
    ->Range(64, kPoolCapacity);

// --- Heap fallback -----------------------------------------------------------

// Deliberately hold more nodes live than the pool has slots: the first
// kPoolCapacity allocations come from the ring, the overflow falls through to
// new T(), and free() routes each pointer back by address range. Quantifies the
// cost cliff when a pool is undersized for its peak — motivation for sizing the
// pool to worst-case book depth rather than the common case.
void BM_ObjectPool_ST_Overflow(benchmark::State &state) {
    const auto n = static_cast<std::uint32_t>(state.range(0));
    object_pool<pooled_order> pool(kPoolCapacity); // n > capacity forces fallback
    std::vector<pooled_order *> held(n, nullptr);

    for (auto _ : state) {
        for (std::uint32_t i = 0; i < n; ++i) {
            held[i] = pool.allocate();
            benchmark::DoNotOptimize(held[i]);
        }
        for (std::uint32_t i = 0; i < n; ++i) pool.free(held[i]);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

BENCHMARK(BM_ObjectPool_ST_Overflow)
    ->Arg(kPoolCapacity * 2)
    ->Arg(kPoolCapacity * 4);

// --- Per-thread pools across cores -------------------------------------------

// The pool is single-threaded by design (one per book side), so the interesting
// multi-core question is not "how does a shared pool contend" but "does the
// design scale when each core owns its own pool." Each benchmark thread builds
// its own object_pool (a thread-local automatic) and hammers only that — no
// sharing, no atomics, no cross-core coherency on the allocator itself.
// items_per_second should stay ~flat per thread (near-linear aggregate scaling);
// any drop is memory-bandwidth / allocator-independent contention, not the pool.
// UseRealTime because wall-clock is the throughput signal across threads.
void BM_ObjectPool_PerThreadPool_AllocFree(benchmark::State &state) {
    object_pool<pooled_order> pool(kPoolCapacity); // one pool per benchmark thread

    for (auto _ : state) {
        pooled_order *obj = pool.allocate();
        benchmark::DoNotOptimize(obj);
        obj->id = 1;
        benchmark::DoNotOptimize(obj->id);
        pool.free(obj);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ObjectPool_PerThreadPool_AllocFree)->ThreadRange(1, 16)->UseRealTime();

} // namespace
