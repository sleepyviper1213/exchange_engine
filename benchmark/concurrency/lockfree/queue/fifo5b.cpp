#include "third_party/fifo.hpp"
#include "utils.hpp"

#include <benchmark/benchmark.h>

namespace {
using namespace utils;

template <typename T>
void BM_Fifo_ST(benchmark::State &state) {
	Fifo5b<T> queue(kQueueCapacity);

	T value{};
	T out{};

	for (auto _ : state) {
		benchmark::DoNotOptimize(queue.push(value++));

		benchmark::DoNotOptimize(queue.pop(out));

		benchmark::DoNotOptimize(out);
	}
}

BENCHMARK(BM_Fifo_ST<int>);

template <typename T>
void BM_Fifo_MT(benchmark::State &state) {
	Fifo5b<T> queue(kQueueCapacity);

	std::atomic<bool> done{false};

	auto producer = spawn_single_producer<T>(done, [&queue](const T &value) {
		return queue.push(value);
	});

	for (T value{}; auto _ : state) {
		while (!queue.pop(value)) {}

		benchmark::DoNotOptimize(value);
	}

	stop_producer<T>(done, producer, [&queue](T &out) {
		return queue.pop(out);
	});

	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_Fifo_MT<int>);
} // namespace
