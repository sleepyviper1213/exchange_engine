#include "core/concurrency/lockfree/spsc_queue.hpp"

#include "queue.fixture.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <numeric>
#include <vector>

namespace {
using namespace utils;
using exchange::core::concurrency::lockfree::spsc_queue;

template <typename T>
std::vector<T> make_payload(size_t batch) {
	std::vector<T> payload(batch);
	std::iota(payload.begin(), payload.end(), T{});
	return payload;
}

template <typename Queue, typename T>
std::thread spawn_batch_producer(Queue &queue, std::atomic<bool> &done,
								 size_t batch) {
	return std::thread{[&, payload = make_payload<T>(batch)] {
		(void)bench_cores().pin_this_thread_to("producer");

		while (!done.load(std::memory_order_acquire)) {
			while (!queue.try_emplace_range(payload))
				if (done.load(std::memory_order_acquire)) return;
		}
	}};
}

template <typename T>
void BM_SPSC_ST_Optional(benchmark::State &state) {
	spsc_queue<T, kQueueCapacity> queue;

	for (T value{}; auto _ : state) {
		benchmark::DoNotOptimize(queue.try_emplace(value++));

		auto item = queue.try_dequeue();

		benchmark::DoNotOptimize(item);
	}
}

BENCHMARK(BM_SPSC_ST_Optional<int>);

template <typename T>
void BM_SPSC_ST_OutParam(benchmark::State &state) {
	spsc_queue<T, kQueueCapacity> queue;

	T value{};
	T out{};

	for (auto _ : state) {
		benchmark::DoNotOptimize(queue.try_emplace(value++));

		benchmark::DoNotOptimize(queue.try_dequeue(out));

		benchmark::DoNotOptimize(out);
	}
}

BENCHMARK(BM_SPSC_ST_OutParam<int>);

template <typename T>
void BM_SPSC_MT_OneByOne(benchmark::State &state) {
	pin_consumer_thread();

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<T>(done, [&queue](const T &value) {
		return queue.try_emplace(value);
	});

	for (T value{}; auto _ : state) {
		while (!queue.try_dequeue(value)) {}

		benchmark::DoNotOptimize(value);
	}

	stop_producer<T>(done, producer, [&queue](T &out) noexcept {
		return queue.try_dequeue(out);
	});

	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_SPSC_MT_OneByOne<int>);

// One-at-a-time consumer fed by a BATCHED producer. Contrast with OneByOne
// (single producer): the only change is the producer publishing one
// write_position_ store per batch, so this isolates how much the consumer's
// write-cursor cache saves over a per-item cross-core acquire load.
template <typename T>
void BM_SPSC_MT_BatchPush(benchmark::State &state) {
	pin_consumer_thread();

	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);

	for (T value{}; auto _ : state) {
		for (size_t i = 0; i < batch; ++i) {
			while (!queue.try_dequeue(value)) {}

			benchmark::DoNotOptimize(value);
		}
	}

	stop_producer<T>(done, producer, [&queue](T &out) noexcept {
		return queue.try_dequeue(out);
	});

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_MT_BatchPush<int>)
->Range(16, 1024);

// Range-pop consumer draining a ONE-BY-ONE producer: how well the bulk
// try_dequeue_range copy-out drains a producer that cannot pre-batch. Pairs
// with BM_SPSC_MT_BatchPushBatchPop (same consumer, batched producer) to
// isolate the producer-batching contribution. Reports the actual popped count,
// since a slow producer makes partial pops the norm here.
template <typename T>
void BM_SPSC_MT_BatchPopRange(benchmark::State &state) {
	pin_consumer_thread();

	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<T>(done, [&queue](const T &value) {
		return queue.try_emplace(value);
	});

	int64_t items = 0;

	for (std::vector<T> buffer(batch); auto _ : state) {
		size_t popped = 0;

		do { popped = queue.try_dequeue_range(buffer); } while (popped == 0);

		benchmark::DoNotOptimize(buffer.data());

		items += static_cast<int64_t>(popped);
	}

	stop_producer<T>(done, producer, [&queue](T &out) noexcept {
		return queue.try_dequeue(out);
	});

	state.SetItemsProcessed(items);
}

BENCHMARK(BM_SPSC_MT_BatchPopRange<int>)
->Range(16, 1024);

// In-place consumer draining a ONE-BY-ONE producer. The callback XORs each
// element into a sink kept live with DoNotOptimize, so the per-element read
// cannot be elided; an empty callback would let the drain collapse to a cursor
// bump and overstate throughput. Pairs with BM_SPSC_MT_BatchPushConsumeUpTo.
template <typename T>
void BM_SPSC_MT_ConsumeUpTo(benchmark::State &state) {
	pin_consumer_thread();

	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<T>(done, [&queue](const T &value) {
		return queue.try_emplace(value);
	});

	int64_t items = 0;

	for (T sink{}; auto _ : state) {
		size_t consumed = 0;

		do {
			consumed = queue.consume_up_to(batch, [&sink](T &value) noexcept {
				sink ^= value;
			});
		} while (consumed == 0);

		benchmark::DoNotOptimize(sink);

		items += static_cast<int64_t>(consumed);
	}

	stop_producer<T>(done, producer, [&queue](T &out) noexcept {
		return queue.try_dequeue(out);
	});

	state.SetItemsProcessed(items);
}

BENCHMARK(BM_SPSC_MT_ConsumeUpTo<int>)
->Range(16, 1024);

template <typename T>
void BM_SPSC_ST_ConsumeAll(benchmark::State &state) {
	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;
	const auto payload = make_payload<T>(batch);
	T sink{};

	for (auto _ : state) {
		queue.clear();

		benchmark::DoNotOptimize(queue.try_emplace_range(payload));

		// XOR sink keeps each element load observable; an empty callback would
		// let consume_all be optimised down to a cursor bump.
		benchmark::DoNotOptimize(
			queue.consume_all([&sink](T &value) noexcept { sink ^= value; }));

		benchmark::DoNotOptimize(sink);
	}

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_ST_ConsumeAll<int>)
->Range(16, 1024);

// Range-pop consumer draining a BATCHED producer: both sides batched, the
// full-throughput pipeline. Pairs with BM_SPSC_MT_BatchPopRange (same
// consumer, one-by-one producer).
template <typename T>
void BM_SPSC_MT_BatchPushBatchPop(benchmark::State &state) {
	pin_consumer_thread();

	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);
	std::vector<T> buffer(batch);


	int64_t items = 0;

	for (T sink{}; auto _ : state) {
		size_t popped = 0;

		do { popped = queue.try_dequeue_range(buffer); } while (popped == 0);

		benchmark::DoNotOptimize(buffer.data());

		items += static_cast<int64_t>(popped);
		for (size_t i = 0; i < popped; ++i) sink ^= buffer[i];

		benchmark::DoNotOptimize(sink);
	}
	stop_producer<T>(done, producer, [&queue](T &out) noexcept {
		return queue.try_dequeue(out);
	});

	state.SetItemsProcessed(items);
}

BENCHMARK(BM_SPSC_MT_BatchPushBatchPop<int>)
->Range(16, 1024);

// In-place consumer draining a BATCHED producer: both sides batched with no
// copy-out. Pairs with BM_SPSC_MT_ConsumeUpTo (same consumer, one-by-one
// producer). Same non-elidable XOR sink so the reads are real work.
template <typename T>
void BM_SPSC_MT_BatchPushConsumeUpTo(benchmark::State &state) {
	pin_consumer_thread();

	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);

	int64_t items = 0;

	for (T sink{}; auto _ : state) {
		size_t consumed = 0;

		do {
			consumed = queue.consume_up_to(batch, [&sink](T &value) noexcept {
				sink ^= value;
			});

		} while (consumed == 0);

		benchmark::DoNotOptimize(sink);

		items += static_cast<int64_t>(consumed);
	}

	stop_producer<T>(done, producer, [&queue](T &out) noexcept {
		return queue.try_dequeue(out);
	});

	state.SetItemsProcessed(items);
}

BENCHMARK(BM_SPSC_MT_BatchPushConsumeUpTo<int>)
->Range(16, 1024);
} // namespace
