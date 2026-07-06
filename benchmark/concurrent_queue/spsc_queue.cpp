#include <benchmark/benchmark.h>
#include "concurrent_queue/fast_queue.hpp"
#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "concurrent_queue/spsc_queue.hpp"
#include "concurrent_queue/fifo.hpp"

namespace {
    /// @brief Ring-buffer element capacity shared by the SPSC benchmarks.
    /// @note A power of two so @c Fifo5b's mask-based indexing is valid and both
    /// queues expose the same effective capacity for a like-for-like comparison.
    inline constexpr size_t kQueueCapacity = 1UL << 14UL;

    /// @brief SPSC throughput: a producer spins pushing integers while the timed
    /// consumer pops exactly one item per benchmark iteration.
    /// @details One iteration == one dequeue, so the framework's own
    /// iterations()/time ratio is the real per-item cost and SetItemsProcessed
    /// simply reports iterations(). The producer runs continuously until the
    /// measured loop ends, then is unblocked by the @c done flag.
    template<class Q>
    void BM_SPSC_Queue_MultiThreaded_Throughput(benchmark::State &state) {
        spsc_queue<Q, kQueueCapacity> queue;
        std::atomic<bool> done{false};

        std::thread producer([&] {
            for (Q i = 0; !done.load(std::memory_order_acquire); ++i) {
                while (!queue.try_emplace(i)) {
                    // Queue full: bail out promptly once the consumer is done.
                    if (done.load(std::memory_order_acquire)) { return; }
                }
            }
        });

        for (auto _: state) {
            std::optional<Q> value;
            while (!(value = queue.try_pop()).has_value()) {
                // Spin until the producer makes an item available.
            }
            benchmark::DoNotOptimize(value);
        }

        done.store(true, std::memory_order_release);
        // Drain any backlog so a producer blocked on a full queue can exit.
        while (queue.try_pop().has_value()) {
        }
        producer.join();
        state.SetItemsProcessed(state.iterations());
    }

    BENCHMARK(BM_SPSC_Queue_MultiThreaded_Throughput<int>)->Unit(
        benchmark::kNanosecond)->Repetitions(10)->DisplayAggregatesOnly(true);

    /// @brief Fifo5b counterpart of the single-item throughput benchmark above,
    /// structured identically so the two numbers are directly comparable.
    /// @details Same @c kQueueCapacity, element type and producer/consumer
    /// shape; only the queue type and its @c push / @c pop API differ. Fifo5b
    /// has no batch-enqueue API, so only this single-item path is compared.
    template<class Q>
    void BM_Fifo5b_MultiThreaded_Throughput(benchmark::State &state) {
        Fifo5b<Q> queue{kQueueCapacity};
        std::atomic<bool> done{false};

        std::thread producer([&] {
            for (Q i = 0; !done.load(std::memory_order_acquire); ++i) {
                while (!queue.push(i)) {
                    // Queue full: bail out promptly once the consumer is done.
                    if (done.load(std::memory_order_acquire)) { return; }
                }
            }
        });

        for (auto _: state) {
            Q value{};
            while (!queue.pop(value)) {
            }
            benchmark::DoNotOptimize(value);
        }

        done.store(true, std::memory_order_release);
        // Drain any backlog so a producer blocked on a full queue can exit.
        for (Q sink{}; queue.pop(sink);) {
        }
        producer.join();
        state.SetItemsProcessed(state.iterations());
    }

    BENCHMARK(BM_Fifo5b_MultiThreaded_Throughput<int>)->Unit(
        benchmark::kNanosecond)->Repetitions(10)->DisplayAggregatesOnly(true);


    template<class Q>
    void BM_SPSC_Queue_SingleThread_RoundTrip(benchmark::State &state) {
        spsc_queue<Q, kQueueCapacity> queue;
        Q i = 0;
        for (auto _: state) {
            benchmark::DoNotOptimize(queue.try_emplace(i++));
            std::optional<Q> value = queue.try_pop();
            benchmark::DoNotOptimize(value);
        }
    }

    template<class Q>
    void BM_SPSC_Queue_OutParam_SingleThread_RoundTrip(benchmark::State &state) {
        spsc_queue<Q, kQueueCapacity> queue;
        Q i = 0;
        for (auto _: state) {
            benchmark::DoNotOptimize(queue.try_emplace(i++));
            Q value{};
            benchmark::DoNotOptimize(queue.try_pop(value));
            benchmark::DoNotOptimize(value);
        }
    }

    BENCHMARK(BM_SPSC_Queue_OutParam_SingleThread_RoundTrip<int>)->Unit(
        benchmark::kNanosecond)->Repetitions(10)->DisplayAggregatesOnly(true);

    /// @brief Fifo5b counterpart of the single-threaded round-trip above.
    template<class Q>
    void BM_Fifo5b_SingleThread_RoundTrip(benchmark::State &state) {
        Fifo5b<Q> queue{kQueueCapacity};
        Q i = 0;
        for (auto _: state) {
            benchmark::DoNotOptimize(queue.push(i++));
            Q value{};
            benchmark::DoNotOptimize(queue.pop(value));
            benchmark::DoNotOptimize(value);
        }
    }

    BENCHMARK(BM_Fifo5b_SingleThread_RoundTrip<int>)->Unit(
        benchmark::kNanosecond)->Repetitions(10)->DisplayAggregatesOnly(true);


    template<class Q>
    void BM_SPSC_Queue_BatchEmplaceRange_Throughput(benchmark::State &state) {
        const auto batch = static_cast<size_t>(state.range(0));

        spsc_queue<Q, kQueueCapacity> queue;
        std::atomic<bool> done{false};

        std::thread consumer([&] {
            while (!done.load(std::memory_order_acquire)) {
                while (queue.try_pop().has_value()) {
                }
            }
            while (queue.try_pop().has_value()) {
            }
        });

        std::vector<Q> payload(batch);
        for (size_t i = 0; i < batch; ++i) {
            payload[i] = static_cast<Q>(i);
        }

        for (auto _: state) {
            while (!queue.try_emplace_range(payload)) {
            }
        }

        done.store(true, std::memory_order_release);
        consumer.join();
        state.SetItemsProcessed(state.iterations() *
                                static_cast<int64_t>(batch));
    }


    BENCHMARK(BM_SPSC_Queue_BatchEmplaceRange_Throughput<int>)
            ->Range(16, 1024)
            ->Unit(benchmark::kNanosecond)
            ->Repetitions(10)->DisplayAggregatesOnly(true);
}
