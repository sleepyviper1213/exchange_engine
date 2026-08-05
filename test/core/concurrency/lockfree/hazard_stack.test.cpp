#include "core/concurrency/lockfree/stack.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace {
using exchange::core::concurrency::lockfree::stack;

// --------------------------------------------------------------------------
// Treiber stack — single threaded correctness
// --------------------------------------------------------------------------

TEST(HazardStack, PopOnEmptyReturnsNullopt) {
	stack<int> s;
	EXPECT_FALSE(s.pop().has_value());
}

TEST(HazardStack, LifoOrder) {
	stack<int> s;
	s.push(1);
	s.push(2);
	s.push(3);
	EXPECT_EQ(s.pop(), 3);
	EXPECT_EQ(s.pop(), 2);
	EXPECT_EQ(s.pop(), 1);
	EXPECT_FALSE(s.pop().has_value());
}

// --------------------------------------------------------------------------
// Treiber stack — concurrent correctness. These are designed to expose
// *concurrency* defects, not throughput: a broken hazard-pointer reclamation
// shows up as a use-after-free (crash under a sanitizer), a lost element, or a
// value popped twice (an ABA/double-reclaim symptom). We never call gtest
// EXPECT/ASSERT from a worker thread — those macros are not thread-safe — so
// each worker records anomalies into atomics and the main thread asserts.
// --------------------------------------------------------------------------


// Outcome of one producers+consumers round: every pushed value must be popped
// exactly once. The in-flight counters are atomic so workers can update them
// race-free; never_popped is tallied on the main thread once everyone joins.
struct stack_run_result {
	std::atomic<int> popped{0};         // total successful pops
	std::atomic<int> double_pops{0};    // a value observed popped more than once
	std::atomic<int> corrupt_values{0}; // a popped value outside the valid range
	int never_popped = 0;               // pushed but never handed to a consumer
};

// Run one multi-producer / multi-consumer round over a fresh stack. Repeated by
// the caller: the race window is small, so a single pass rarely trips a latent
// bug — many short rounds are far more likely to catch it than one long pass.
void run_concurrent_round(int producers, int consumers, int per_producer,
                          stack_run_result &result) {
	const int total = producers * per_producer;
	stack<int> s;
	std::atomic<bool> producers_done{false};
	std::vector<std::atomic<std::uint8_t> > seen(
		static_cast<std::size_t>(total));
	for (auto &flag : seen) flag.store(0, std::memory_order_relaxed);

	std::vector<std::thread> threads;
	threads.reserve(static_cast<std::size_t>(producers) +
					static_cast<std::size_t>(consumers));
	for (int p = 0; p < producers; ++p) {
		threads.emplace_back([&, p] {
			for (int i = 0; i < per_producer; ++i) s.push((p * per_producer) + i);
		});
	}
	for (int c = 0; c < consumers; ++c) {
		threads.emplace_back([&] {
			while (true) {
				std::optional<int> v = s.pop();
				if (!v) {
					// Producers are joined before producers_done is set, and
					// pop() only reports empty after observing a null head, so
					// past that point the stack can never refill. Exiting on
					// *observed empty* rather than on a pop count is what keeps
					// this terminating: a lost or corrupted element must fail
					// an assertion below, never wedge the consumers in a spin
					// waiting for a count that can no longer be reached.
					if (producers_done.load(std::memory_order_acquire)) break;
					continue;
				}
				if (*v < 0 || *v >= total) {
					result.corrupt_values.fetch_add(1,
					                                std::memory_order_relaxed);
					continue;
				}
				// A second observer of the same value means the node was
				// handed out twice — the exact failure hazard pointers
				// exist to prevent.
				if (seen[static_cast<std::size_t>(*v)].fetch_add(
					    1,
					    std::memory_order_relaxed) != 0) {
					result.double_pops.fetch_add(1, std::memory_order_relaxed);
				}
				result.popped.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	for (int p = 0; p < producers; ++p)
		threads[static_cast<std::size_t>(p)].join();
	producers_done.store(true, std::memory_order_release);
	for (int c = producers; c < producers + consumers; ++c)
		threads[static_cast<std::size_t>(c)].join();

	// Conservation checked against the per-value flags, not the pop tally: a
	// double-pop inflates the tally by exactly as much as a lost element
	// deflates it, so counting alone can net out to "correct" on a run that
	// both duplicated and dropped elements.
	for (const auto &flag : seen)
		if (flag.load(std::memory_order_relaxed) == 0) ++result.never_popped;
}

TEST(HazardStack, ConcurrentPushPopConservesElements) {
	// Many short rounds rather than one long run: repetition is what turns a
	// rare interleaving into a reproducible failure.
	constexpr int kRounds      = 50;
	constexpr int kProducers   = 4;
	constexpr int kConsumers   = 4;
	constexpr int kPerProducer = 4000;
	constexpr int kTotal       = kProducers * kPerProducer;

	for (int round = 0; round < kRounds; ++round) {
		stack_run_result result;
		run_concurrent_round(kProducers, kConsumers, kPerProducer, result);

		ASSERT_EQ(result.corrupt_values.load(), 0)
			<< "round " << round
			<< ": popped an out-of-range value "
			   "(memory corruption / use-after-free)";
		ASSERT_EQ(result.double_pops.load(), 0)
			<< "round " << round
			<< ": a value was popped more than once "
			   "(node reclaimed while still reachable)";
		ASSERT_EQ(result.never_popped, 0)
			<< "round " << round << ": pushed values were never popped";
		ASSERT_EQ(result.popped.load(), kTotal)
			<< "round " << round << ": pop tally does not match what was pushed";
	}
}

// Each thread both pushes and pops, so producers and consumers race on the same
// nodes continuously — the interleaving that most aggressively exercises
// hazard-pointer reclamation (a popped node being retired while another thread
// is mid-traversal of it).
TEST(HazardStack, ConcurrentMixedPushPopIsMemorySafe) {
	constexpr int THREAD_COUNT = 8;
	constexpr int OPS_EACH = 20000;
	constexpr int MAX_VALUE = THREAD_COUNT * OPS_EACH;

	stack<int> s;
	std::atomic<int> corrupt{0};

	std::vector<std::thread> threads;
	threads.reserve(THREAD_COUNT);
	for (int t = 0; t < THREAD_COUNT; ++t) {
		threads.emplace_back([&, t] {
			for (int i = 0; i < OPS_EACH; ++i) {
				s.push((t * OPS_EACH) + i);
				if (std::optional<int> v = s.pop()) {
					// Checked against the whole pushed range, not just for
					// negativity: reading a freed node most often yields a
					// plausible-looking positive integer, so "v >= 0" would let
					// exactly the failure this test exists to catch slip past.
					if (*v < 0 || *v >= MAX_VALUE)
						corrupt.fetch_add(1, std::memory_order_relaxed);
				}
			}
		});
	}
	for (auto &thread : threads) thread.join();

	EXPECT_EQ(corrupt.load(), 0);
	// The stack must be fully drainable afterwards without crashing.
	while (s.pop().has_value()) {}
}
} // namespace