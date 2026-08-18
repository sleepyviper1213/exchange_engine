#include "third_party/reader_writer_queue.hpp"

#include "queue.fixture.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace {
using namespace utils;

/// @brief Single-block configuration of moodycamel::ReaderWriterQueue.
/// @details The default @c MAX_BLOCK_SIZE (512) makes the lockfree split a
/// @ref kQueueCapacity request into a circular linked list of ~34 blocks and
/// hand the producer/consumer roles across block boundaries at runtime. Sizing
/// @c MAX_BLOCK_SIZE to the full capacity forces a single contiguous block,
/// which (a) matches how spsc_queue and folly::ProducerConsumerQueue are laid
/// out, keeping the comparison apples-to-apples, and (b) sidesteps the
/// multi-block advancement path - the only path exercised here, and the one a
/// MinGW GCC @c -O2 Release build faults on (access violation) under sustained
/// 1P/1C traffic. Debug and single-block builds are stable, which points at
/// optimized codegen around the block hand-off rather than a role misuse in the
/// benchmark harness.
/// @pre @c kQueueCapacity is a power of two (required for @c MAX_BLOCK_SIZE).
template <typename T>
using rwq = moodycamel::ReaderWriterQueue<T>;

// moodycamel::ReaderWriterQueue is the reference lock-free SPSC lockfree this
// project's spsc_queue is measured against. It exposes try_emplace() on the
// producer side (so spawn_single_producer drives it unchanged) and
// try_dequeue() on the consumer side. Only the single-element paths are
// benchmarked here because ReaderWriterQueue has no bulk enqueue/dequeue
// counterpart to spsc_queue's try_emplace_range / try_pop_range; the
// head-to-head cases mirror BM_SPSC_ST_OutParam and BM_SPSC_MT_OneByOne
// exactly.

// Single-threaded ping-pong: one enqueue immediately followed by one dequeue on
// the same thread. Isolates the per-operation instruction cost with no
// cross-core coherency traffic. Compare with BM_SPSC_ST_OutParam.
template <typename T>
void BM_RWQ_ST(benchmark::State &state) {
	rwq<T> queue(kQueueCapacity);

	for (T value{}, out{}; auto _ : state) {
		benchmark::DoNotOptimize(queue.try_emplace(value++));

		benchmark::DoNotOptimize(queue.try_dequeue(out));

		benchmark::DoNotOptimize(out);
	}
}

BENCHMARK(BM_RWQ_ST<int>);

// Cross-core one-at-a-time: a pinned producer enqueues while the pinned
// consumer dequeues one element per iteration, paying real inter-core cache
// coherency. Compare with BM_SPSC_MT_OneByOne.
template <typename T>
void BM_RWQ_MT_OneByOne(benchmark::State &state) {
	rwq<T> queue(kQueueCapacity);

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<T>(done, [&queue](const T &value) {
		return queue.try_enqueue(value);
	});

	for (T value{}; auto _ : state) {
		while (!queue.try_dequeue(value)) {}

		benchmark::DoNotOptimize(value);
	}

	stop_producer<T>(done, producer, [&queue](T &out) {
		return queue.try_dequeue(out);
	});

	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_RWQ_MT_OneByOne<int>);
} // namespace
