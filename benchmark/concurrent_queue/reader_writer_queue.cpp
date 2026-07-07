#include "concurrent_queue/reader_writer_queue.hpp"

#include "utils.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace {
using namespace utils;

// moodycamel::ReaderWriterQueue is the reference lock-free SPSC queue this
// project's spsc_queue is measured against. It exposes try_emplace() on the
// producer side (so spawn_single_producer drives it unchanged) and try_dequeue()
// on the consumer side. Only the single-element paths are benchmarked here
// because ReaderWriterQueue has no bulk enqueue/dequeue counterpart to
// spsc_queue's try_emplace_range / try_pop_range; the head-to-head cases mirror
// BM_SPSC_ST_OutParam and BM_SPSC_MT_OneByOne exactly.

// Drain the queue after the timed loop and join the producer. Mirrors
// stop_producer in spsc_queue.cpp but spelled against try_dequeue, since
// ReaderWriterQueue has no optional-returning pop.
template <typename Queue, typename T>
void stop_producer(Queue &queue, std::atomic<bool> &done,
				   std::thread &producer) {
	done.store(true, std::memory_order_release);

	for (T sink{}; queue.try_dequeue(sink);) {}

	producer.join();
}

// Single-threaded ping-pong: one enqueue immediately followed by one dequeue on
// the same thread. Isolates the per-operation instruction cost with no
// cross-core coherency traffic. Compare with BM_SPSC_ST_OutParam.
template <typename T>
void BM_RWQ_ST(benchmark::State &state) {
	moodycamel::ReaderWriterQueue<T> queue(kQueueCapacity);

	T value{};
	T out{};

	for (auto _ : state) {
		benchmark::DoNotOptimize(queue.try_emplace(value++));

		benchmark::DoNotOptimize(queue.try_dequeue(out));

		benchmark::DoNotOptimize(out);
	}
}

BENCHMARK(BM_RWQ_ST<int>);

// Cross-core one-at-a-time: a pinned producer enqueues while the pinned consumer
// dequeues one element per iteration, paying real inter-core cache coherency.
// Compare with BM_SPSC_MT_OneByOne.
template <typename T>
void BM_RWQ_MT_OneByOne(benchmark::State &state) {
	moodycamel::ReaderWriterQueue<T> queue(kQueueCapacity);

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<decltype(queue), T>(queue, done);

	if (!pin_current_thread_to_core(kConsumerCore))
		state.SetLabel("consumer-unpinned");

	T value{};

	for (auto _ : state) {
		while (!queue.try_dequeue(value)) {}

		benchmark::DoNotOptimize(value);
	}

	stop_producer<decltype(queue), T>(queue, done, producer);

	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_RWQ_MT_OneByOne<int>);
} // namespace
