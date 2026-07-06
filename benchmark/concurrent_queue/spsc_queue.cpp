#include "concurrent_queue/spsc_queue.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

namespace {

inline constexpr size_t kQueueCapacity = 1u << 14;

template <typename T>
std::vector<T> make_payload(size_t batch) {
	std::vector<T> payload(batch);
	std::iota(payload.begin(), payload.end(), T{});
	return payload;
}

template <typename Queue, typename T>
std::thread spawn_single_producer(Queue &queue, std::atomic<bool> &done) {
	return std::thread([&] {
		for (T value{};; ++value) {
			if (done.load(std::memory_order_acquire)) return;

			while (!queue.try_emplace(value))

				if (done.load(std::memory_order_acquire)) return;
		}
	});
}

template <typename Queue, typename T>
std::thread spawn_batch_producer(Queue &queue, std::atomic<bool> &done,
								 size_t batch) {
	return std::thread([&, payload = make_payload<T>(batch)] {
		while (!done.load(std::memory_order_acquire)) {
			while (!queue.try_emplace_range(payload))

				if (done.load(std::memory_order_acquire)) return;
		}
	});
}

template <typename Queue, typename T>
std::thread spawn_fifo_producer(Queue &queue, std::atomic<bool> &done) {
	return std::thread([&] {
		for (T value{};; ++value) {
			if (done.load(std::memory_order_acquire)) return;

			while (!queue.push(value))

				if (done.load(std::memory_order_acquire)) return;
		}
	});
}

template <typename T>
static void BM_SPSC_ST_Optional(benchmark::State &state) {
	spsc_queue<T, kQueueCapacity> queue;

	T value{};

	for (auto _ : state) {
		benchmark::DoNotOptimize(queue.try_emplace(value++));

		auto item = queue.try_pop();

		benchmark::DoNotOptimize(item);
	}
}

BENCHMARK(BM_SPSC_ST_Optional<int>);

template <typename T>
static void BM_SPSC_ST_OutParam(benchmark::State &state) {
	spsc_queue<T, kQueueCapacity> queue;

	T value{};
	T out{};

	for (auto _ : state) {
		benchmark::DoNotOptimize(queue.try_emplace(value++));

		benchmark::DoNotOptimize(queue.try_pop(out));

		benchmark::DoNotOptimize(out);
	}
}

BENCHMARK(BM_SPSC_ST_OutParam<int>);

template <typename Queue>
void stop_producer(Queue &queue, std::atomic<bool> &done,
				   std::thread &producer) {
	done.store(true, std::memory_order_release);

	while (queue.try_pop().has_value()) {}

	producer.join();
}

template <typename T>
static void BM_SPSC_MT_OneByOne(benchmark::State &state) {
	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<decltype(queue), T>(queue, done);

	T value{};

	for (auto _ : state) {
		while (!queue.try_pop(value)) {}

		benchmark::DoNotOptimize(value);
	}

	stop_producer(queue, done, producer);

	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_SPSC_MT_OneByOne<int>);

template <typename T>
static void BM_SPSC_MT_BatchPush(benchmark::State &state) {
	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);

	T value{};

	for (auto _ : state) {
		for (size_t i = 0; i < batch; ++i) {
			while (!queue.try_pop(value)) {}

			benchmark::DoNotOptimize(value);
		}
	}

	stop_producer(queue, done, producer);

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_MT_BatchPush<int>)
->RangeMultiplier(2)->Range(16, 1024);

template <typename T>
static void BM_SPSC_MT_BatchPopRange(benchmark::State &state) {
	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);

	std::vector<T> buffer(batch);

	for (auto _ : state) {
		size_t popped;

		do { popped = queue.try_pop_range(buffer); } while (popped == 0);

		benchmark::DoNotOptimize(buffer.data());
	}

	stop_producer(queue, done, producer);

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_MT_BatchPopRange<int>)
->RangeMultiplier(2)->Range(16, 1024);

template <typename T>
static void BM_SPSC_MT_ConsumeUpTo(benchmark::State &state) {
	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);

	for (auto _ : state) {
		size_t consumed = 0;

		do {
			consumed = queue.consume_up_to(batch, [](T &) noexcept {});

		} while (consumed == 0);

		benchmark::ClobberMemory();
	}

	stop_producer(queue, done, producer);

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_MT_ConsumeUpTo<int>)
->RangeMultiplier(2)->Range(16, 1024);

template <typename T>
static void BM_SPSC_ST_ConsumeAll(benchmark::State &state) {
	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	auto payload = make_payload<T>(batch);

	for (auto _ : state) {
		queue.clear();

		benchmark::DoNotOptimize(queue.try_emplace_range(payload));

		benchmark::DoNotOptimize(queue.consume_all([](T &) noexcept {}));
	}

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_ST_ConsumeAll<int>)
->RangeMultiplier(2)->Range(16, 1024);

template <typename T>
static void BM_SPSC_MT_BatchPushBatchPop(benchmark::State &state) {
	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);

	std::vector<T> buffer(batch);

	for (auto _ : state) {
		size_t popped;

		do { popped = queue.try_pop_range(buffer); } while (popped == 0);

		benchmark::DoNotOptimize(buffer.data());
	}

	stop_producer(queue, done, producer);

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_MT_BatchPushBatchPop<int>)
->RangeMultiplier(2)->Range(16, 1024);

template <typename T>
static void BM_SPSC_MT_BatchPushConsumeUpTo(benchmark::State &state) {
	const size_t batch = state.range(0);

	spsc_queue<T, kQueueCapacity> queue;

	std::atomic<bool> done{false};

	auto producer =
		spawn_batch_producer<decltype(queue), T>(queue, done, batch);

	for (auto _ : state) {
		size_t consumed;

		do {
			consumed = queue.consume_up_to(batch, [](T &) noexcept {});

		} while (consumed == 0);

		benchmark::ClobberMemory();
	}

	stop_producer(queue, done, producer);

	state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

BENCHMARK(BM_SPSC_MT_BatchPushConsumeUpTo<int>)
->RangeMultiplier(2)->Range(16, 1024);
} // namespace
