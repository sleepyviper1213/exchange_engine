#include "event/command.hpp"
#include "execution/engine_partition.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include<spdlog/stopwatch.h>
using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange;

// Throughput of the staged MatchingEngine: how fast commands flow through the
// SPSC lockfree and get applied to the book. Single-threaded (producer and
// consumer on one core), so this is the dispatch/matching ceiling without
// cross-core cache-line traffic - the two-thread handoff is a separate concern.
namespace {

// A 4096-slot inline ring - 0.19 MB at sizeof(command) == 48, so it sits inside
// L2 and the engine is heap-allocated only to keep it off the stack.
//
// The capacity is load-bearing for TwoThreadPipeline below, not just an
// implementation detail: a pass whose command batch fits the ring runs the
// producer and consumer concurrently to completion, while one that overruns it
// makes the producer spin on a full ring against the consumer's read cursor.
// Measured, raising this to 1U << 16 moved n=32768 from 7.9 to 10.2 M/s (it now
// fits) and left n=262144 at ~7.4 M/s (it still does not) - while costing the
// single-threaded MatchThroughput sweep ~19%, because a 3 MB ring no longer
// fits L2. Ring size is a trade between the two, so this stays where the rest
// of the suite was measured.
using Engine = execution::engine_partition<1U << 12>;

// Self-cancelling crossing pairs: an ASK rests at a price, then a BID at the
// same price and size fully consumes it - so the book returns to empty after
// every pair and memory stays bounded across iterations, while still exercising
// match, rest, pop_front and the level insert/erase at varied sorted positions.
std::vector<command> makeCrossingPairs(std::size_t n) {
	std::mt19937_64 rng(42);
	// Not named `price`: that would shadow the type for the rest of the scope.
	std::uniform_int_distribution<price_t> price_dist(1, 100'000);
	std::vector<command> cmds;
	cmds.reserve(n);
	for (std::size_t i = 0; i < n; i += 2) {
		const price_t price      = price_dist(rng);
		constexpr quantity_t qty = 10;
		cmds.push_back(command::place(order{.id    = i + 1,
											.side  = side_t::ask,
											.price = price,
											.qty   = qty}));
		cmds.push_back(command::place(order{.id    = i + 2,
											.side  = side_t::bid,
											.price = price,
											.qty   = qty}));
	}
	return cmds;
}

// Cancels of ids that were never placed: the book work is a single failed hash
// lookup, so this isolates the lockfree + drain-loop + dispatch cost from
// matching.
std::vector<command> makeNoopCancels(std::size_t n) {
	std::vector<command> cmds;
	cmds.reserve(n);
	for (std::size_t i = 0; i < n; ++i)
		cmds.push_back(command::cancel(0, static_cast<order_id_t>(i + 1)));
	return cmds;
}

// Push the whole batch through the engine, respecting the bounded lockfree:
// fill until full (or done), drain, repeat.
//
// The record store is cleared first because every pass replays the *same* ids,
// and an id stays spent for as long as the store remembers the order that used
// it - without this, pass two would be 100% DUPLICATE_ORDER_ID and the
// benchmark would be measuring rejections. A pass is a session, and clearing is
// what starts the next one. It costs one pass over the slots actually handed
// out, so it scales with the batch rather than with the store's capacity.
void run(Engine &engine, const std::vector<command> &cmds) {
	engine.orders().clear();
	std::size_t i = 0;
	while (i < cmds.size()) {
		while (i < cmds.size() && engine.submit(cmds[i])) ++i;
		engine.drain_and_flush();
	}
	engine.drain_and_flush();
}

void BM_MatchingEngine_MatchThroughput(benchmark::State &state) {
	const auto n    = static_cast<std::size_t>(state.range(0));
	const auto cmds = makeCrossingPairs(n);
	auto engine     = std::make_unique<Engine>(nullptr);
	engine->listing(0); // one listing; routing is not what this measures

	for (auto _ : state) {
		run(*engine, cmds);
		benchmark::DoNotOptimize(engine.get());
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

BENCHMARK(BM_MatchingEngine_MatchThroughput)
->RangeMultiplier(16)->Range(8, 8 << 12);

void BM_MatchingEngine_QueueThroughput(benchmark::State &state) {
	const auto n    = static_cast<std::size_t>(state.range(0));
	const auto cmds = makeNoopCancels(n);
	auto engine     = std::make_unique<Engine>(nullptr);
	engine->listing(0); // one listing; routing is not what this measures

	for (auto _ : state) {
		run(*engine, cmds);
		benchmark::DoNotOptimize(engine.get());
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

BENCHMARK(BM_MatchingEngine_QueueThroughput)
->RangeMultiplier(16)->Range(8, 8 << 12);

// --- Two-thread pipeline -----------------------------------------------------
//
// The single-threaded numbers keep both ring cursors hot in one core's L1. The
// real deployment splits them: the producer writes the write cursor the
// consumer reads, and vice versa, so each publish bounces a cache line across
// cores. To measure that without polluting the sample with thread-creation
// cost, the producer and consumer threads are spawned once and reused; a
// per-iteration handshake releases both and waits for both to finish, and only
// that concurrent submit/drain window is timed (UseManualTime).

class Pipeline {
public:
	Pipeline(Engine &engine, const std::vector<command> &cmds)
		: engine_(engine),
		  cmds_(cmds),
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
		// Both workers are parked here, so this thread is momentarily the
		// store's single owner and may reset the session - see run() for why
		// every pass needs one. Before the clock starts: it is setup, not
		// handoff cost.
		engine_.orders().clear();
		const spdlog::stopwatch stopwatch;
		done_.store(0, std::memory_order_relaxed);
		start_gen_.fetch_add(1, std::memory_order_release);
		start_gen_.notify_all();
		for (unsigned d = done_.load(std::memory_order_acquire); d < 2;
			 d          = done_.load(std::memory_order_acquire))
			done_.wait(d, std::memory_order_acquire);
		return stopwatch.elapsed().count();
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
	std::atomic<unsigned> done_{0}; ///< workers that finished this pass
	std::atomic<bool> stop_{false};
	std::thread producer_;
	std::thread consumer_;
};

void BM_MatchingEngine_TwoThreadPipeline(benchmark::State &state) {
	const auto n    = static_cast<std::size_t>(state.range(0));
	const auto cmds = makeCrossingPairs(n);
	auto engine     = std::make_unique<Engine>(nullptr);
	engine->listing(0); // one listing; routing is not what this measures
	Pipeline pipe(*engine, cmds);

	for (auto _ : state) state.SetIterationTime(pipe.run_once());
	state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

// Start where the single-thread sweep ends: below a few thousand commands the
// handshake latency, not the handoff, dominates the sample.
BENCHMARK(BM_MatchingEngine_TwoThreadPipeline)
->RangeMultiplier(8)->Range(1 << 12, 1 << 18)->UseManualTime();

} // namespace
