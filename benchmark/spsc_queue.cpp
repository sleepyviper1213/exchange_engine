#include <benchmark/benchmark.h>
#include "concurrent_queue/fast_queue.hpp"
#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "concurrent_queue/SPSCQueue.hpp"

namespace {
    /// @brief Ring-buffer element capacity shared by the SPSC benchmarks.
    inline constexpr size_t kQueueCapacity = 1UL << 14UL;

    /// @brief SPSC throughput: a producer spins pushing integers while the timed
    /// consumer pops exactly one item per benchmark iteration.
    /// @details One iteration == one dequeue, so the framework's own
    /// iterations()/time ratio is the real per-item cost and SetItemsProcessed
    /// simply reports iterations(). The producer runs continuously until the
    /// measured loop ends, then is unblocked by the @c done flag.
    template<class Q>
    void BM_SPSC_Queue_MultiThreaded_Throughput(benchmark::State &state) {
        SPSCQueue<Q, kQueueCapacity> queue;
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

    /// @brief Batch-enqueue throughput of @c try_emplace_range: each timed
    /// iteration bulk-pushes @c state.range(0) items while a background consumer
    /// drains continuously.
    /// @details Every iteration enqueues exactly one full batch (the retry loop
    /// spins on a momentarily-full queue but copies nothing on a failed attempt),
    /// so SetItemsProcessed reports iterations() * batch. This isolates the
    /// producer-side cost — dominated by the two @c memory calls — as opposed to
    /// the per-item pop latency measured above.
    template<class Q>
    void BM_SPSC_Queue_BatchEmplaceRange_Throughput(benchmark::State &state) {
        const auto batch = static_cast<size_t>(state.range(0));

        SPSCQueue<Q, kQueueCapacity> queue;
        std::atomic<bool> done{false};

        std::thread consumer([&] {
            while (!done.load(std::memory_order_acquire)) {
                while (queue.try_pop().has_value()) {
                }
            }
            // Final sweep after the producer stops, so nothing is left dangling.
            while (queue.try_pop().has_value()) {
            }
        });

        std::vector<Q> payload(batch);
        for (size_t i = 0; i < batch; ++i) {
            payload[i] = static_cast<Q>(i);
        }

        for (auto _: state) {
            while (!queue.try_emplace_range(payload)) {
                // Queue momentarily full: let the consumer catch up.
            }
        }

        done.store(true, std::memory_order_release);
        consumer.join();
        state.SetItemsProcessed(state.iterations() *
                                static_cast<int64_t>(batch));
    }

    // Sweep batch sizes from small (amortized call overhead dominates) up to a
    // quarter of the ring (memory-bound); all stay below kQueueCapacity.
    BENCHMARK(BM_SPSC_Queue_BatchEmplaceRange_Throughput<int>)
            ->Range(16, 1024)
            ->Unit(benchmark::kNanosecond)
            ->Repetitions(10)->DisplayAggregatesOnly(true);
}
