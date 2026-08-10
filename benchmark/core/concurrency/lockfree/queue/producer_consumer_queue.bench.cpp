#include "third_party/producer_consumer_queue.hpp"

#include "queue.fixture.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>

namespace {
using namespace utils;

template <typename T>
using pcq = folly::ProducerConsumerQueue<T>;

// folly::ProducerConsumerQueue is the second reference lock-free SPSC lockfree in
// the shoot-out (alongside moodycamel::ReaderWriterQueue). It is a single-ring
// design like spsc_queue rather than moodycamel's lockfree-of-blocks, so it is the
// closest structural peer to this project's lockfree. API: write(args...) on the
// producer, read(out&) on the consumer. Single-element only (no bulk API), so
// only the ST ping-pong and cross-core one-by-one cases are benchmarked,
// matching BM_RWQ_* and BM_SPSC_MT_OneByOne / BM_SPSC_ST_OutParam exactly.

// Single-threaded ping-pong: one write immediately followed by one read on the
// same thread. Isolates per-operation instruction cost with no cross-core
// coherency traffic. Compare with BM_RWQ_ST and BM_SPSC_ST_OutParam.
template <typename T>
void BM_FollyPCQ_ST(benchmark::State &state) {
	pcq<T> queue(kQueueCapacity);

	T value{};
	T out{};

	for (auto _ : state) {
		benchmark::DoNotOptimize(queue.write(value++));

		benchmark::DoNotOptimize(queue.read(out));

		benchmark::DoNotOptimize(out);
	}
}

BENCHMARK(BM_FollyPCQ_ST<int>);

// Cross-core one-at-a-time: a pinned producer writes while the pinned consumer
// reads one element per iteration, paying real inter-core cache coherency.
// Compare with BM_RWQ_MT_OneByOne and BM_SPSC_MT_OneByOne.
template <typename T>
void BM_FollyPCQ_MT_OneByOne(benchmark::State &state) {
	pcq<T> queue(kQueueCapacity);

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<T>(done, [&queue](const T &value) {
		return queue.write(value);
	});

	for (T value{}; auto _ : state) {
		while (!queue.read(value)) {}

		benchmark::DoNotOptimize(value);
	}

	stop_producer<T>(done, producer, [&queue](T &out) {
		return queue.read(out);
	});

	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_FollyPCQ_MT_OneByOne<int>);
} // namespace
