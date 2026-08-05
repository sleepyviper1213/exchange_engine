#include "trading-engine/execution/matching_engine.hpp"

#include "trading-engine/event/command.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange;
// Throughput of the staged MatchingEngine: how fast commands flow through the
// SPSC lockfree and get applied to the book. Single-threaded (producer and consumer
// on one core), so this is the dispatch/matching ceiling without cross-core
// cache-line traffic — the two-thread handoff is a separate concern.
namespace {

// A ~3 MB inline ring: heap-allocate the engine so it never lands on the stack.
using Engine = execution::MatchingEngine<1U << 12>;

// Self-cancelling crossing pairs: an ASK rests at a price, then a BID at the same
// price and size fully consumes it — so the book returns to empty after every
// pair and memory stays bounded across iterations, while still exercising match,
// rest, pop_front and the level insert/erase at varied sorted positions.
std::vector<command> makeCrossingPairs(std::size_t n) {
    std::mt19937_64 rng(42);
    // Not named `price`: that would shadow the type for the rest of the scope.
    std::uniform_int_distribution<price_t> price_dist(1, 100'000);
    std::vector<command> cmds;
    cmds.reserve(n);
    for (std::size_t i = 0; i < n; i += 2) {
        const price_t price = price_dist(rng);
        constexpr quantity_t qty = 10;
        cmds.push_back(command::place(order{
            .id = i + 1, .side = side_t::ask, .price = price, .qty = qty}));
        cmds.push_back(command::place(order{
            .id = i + 2, .side = side_t::bid, .price = price, .qty = qty}));
    }
    return cmds;
}

// Cancels of ids that were never placed: the book work is a single failed hash
// lookup, so this isolates the lockfree + drain-loop + dispatch cost from matching.
std::vector<command> makeNoopCancels(std::size_t n) {
    std::vector<command> cmds;
    cmds.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        cmds.push_back(command::cancel(static_cast<order_id_t>(i + 1)));
    return cmds;
}

// Push the whole batch through the engine, respecting the bounded lockfree: fill
// until full (or done), drain, repeat.
void run(Engine &engine, const std::vector<command> &cmds) {
    std::size_t i = 0;
    while (i < cmds.size()) {
        while (i < cmds.size() && engine.submit(cmds[i])) ++i;
        engine.drain();
    }
    engine.drain();
}

void BM_MatchingEngine_MatchThroughput(benchmark::State &state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto cmds = makeCrossingPairs(n);
    auto engine = std::make_unique<Engine>(nullptr);

    for (auto _: state) {
        run(*engine, cmds);
        benchmark::DoNotOptimize(engine.get());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

BENCHMARK(BM_MatchingEngine_MatchThroughput)
    ->RangeMultiplier(16)
    ->Range(8, 8 << 12);

void BM_MatchingEngine_QueueThroughput(benchmark::State &state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto cmds = makeNoopCancels(n);
    auto engine = std::make_unique<Engine>(nullptr);

    for (auto _: state) {
        run(*engine, cmds);
        benchmark::DoNotOptimize(engine.get());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

BENCHMARK(BM_MatchingEngine_QueueThroughput)
    ->RangeMultiplier(16)
    ->Range(8, 8 << 12);

// --- Two-thread pipeline -----------------------------------------------------
//
// The single-threaded numbers keep both ring cursors hot in one core's L1. The
// real deployment splits them: the producer writes the write cursor the consumer
// reads, and vice versa, so each publish bounces a cache line across cores. To
// measure that without polluting the sample with thread-creation cost, the
// producer and consumer threads are spawned once and reused; a per-iteration
// handshake releases both and waits for both to finish, and only that concurrent
// submit/drain window is timed (UseManualTime).

class Pipeline {
public:
    Pipeline(Engine &engine, const std::vector<command> &cmds)
        : engine_(engine), cmds_(cmds),
          producer_([this] { producer_loop(); }),
          consumer_([this] { consumer_loop(); }) {}

    ~Pipeline() {
        stop_.store(true, std::memory_order_relaxed);
        start_gen_.fetch_add(1, std::memory_order_release);
        start_gen_.notify_all();
        producer_.join();
        consumer_.join();
    }

    /// @brief Release both workers for one pass and return the wall time until
    ///        both report done.
    double run_once() {
        const auto begin = std::chrono::steady_clock::now();
        done_.store(0, std::memory_order_relaxed);
        start_gen_.fetch_add(1, std::memory_order_release);
        start_gen_.notify_all();
        for (unsigned d = done_.load(std::memory_order_acquire); d < 2;
             d = done_.load(std::memory_order_acquire))
            done_.wait(d, std::memory_order_acquire);
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - begin)
            .count();
    }

private:
    // Block until start_gen_ moves past @p seen, then adopt the new value.
    bool wait_turn(std::uint64_t &seen) {
        start_gen_.wait(seen, std::memory_order_acquire);
        seen = start_gen_.load(std::memory_order_acquire);
        return !stop_.load(std::memory_order_relaxed);
    }

    void finish() {
        done_.fetch_add(1, std::memory_order_release);
        done_.notify_one();
    }

    void producer_loop() {
        for (std::uint64_t seen = 0; wait_turn(seen);) {
            for (std::size_t i = 0; i < cmds_.size();)
                while (i < cmds_.size() && engine_.submit(cmds_[i])) ++i;
            finish();
        }
    }

    void consumer_loop() {
        for (std::uint64_t seen = 0; wait_turn(seen);) {
            std::size_t applied = 0;
            while (applied < cmds_.size()) {
                const std::size_t got = engine_.drain();
                if (got == 0) std::this_thread::yield();
                else applied += got;
            }
            finish();
        }
    }

    Engine &engine_;
    const std::vector<command> &cmds_;
    std::atomic<std::uint64_t> start_gen_{0}; ///< bumped once per pass
    std::atomic<unsigned> done_{0};           ///< workers that finished this pass
    std::atomic<bool> stop_{false};
    std::thread producer_;
    std::thread consumer_;
};

void BM_MatchingEngine_TwoThreadPipeline(benchmark::State &state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto cmds = makeCrossingPairs(n);
    auto engine = std::make_unique<Engine>(nullptr);
    Pipeline pipe(*engine, cmds);

    for (auto _: state) {
        state.SetIterationTime(pipe.run_once());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

// Start where the single-thread sweep ends: below a few thousand commands the
// handshake latency, not the handoff, dominates the sample.
BENCHMARK(BM_MatchingEngine_TwoThreadPipeline)
    ->RangeMultiplier(8)
    ->Range(1 << 12, 1 << 18)
    ->UseManualTime();

} // namespace
