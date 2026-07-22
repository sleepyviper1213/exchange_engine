#include "lockfree/stack/stack.hpp"
#include "synchronisation/hazard_pointer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

using core::lockfree::stack;
using core::synchronisation::hazard_pointer;
using core::synchronisation::hazard_pointer_domain;
using core::synchronisation::hazard_pointer_obj_base;
using core::synchronisation::hazard_pointer_array;
using core::synchronisation::make_hazard_pointer;

namespace {

// A retirable object that reports its own destruction, so tests can assert
// exactly when reclamation happens.
struct Tracked : hazard_pointer_obj_base<Tracked> {
	std::atomic<int> *counter;
	int               id;

	Tracked(std::atomic<int> *c, int i) : counter(c), id(i) {}
	~Tracked() { counter->fetch_add(1, std::memory_order_relaxed); }
};

// --------------------------------------------------------------------------
// hazard_pointer handle basics
// --------------------------------------------------------------------------

TEST(HazardPointer, DefaultConstructedIsEmpty) {
	hazard_pointer hp;
	EXPECT_TRUE(hp.empty());
}

TEST(HazardPointer, MakeYieldsNonEmpty) {
	hazard_pointer hp = make_hazard_pointer();
	EXPECT_FALSE(hp.empty());
}

TEST(HazardPointer, MoveTransfersOwnership) {
	hazard_pointer a = make_hazard_pointer();
	hazard_pointer b = std::move(a);
	EXPECT_TRUE(a.empty());
	EXPECT_FALSE(b.empty());
}

TEST(HazardPointer, ProtectReturnsCurrentValue) {
	int             value = 42;
	std::atomic<int *> src{&value};
	hazard_pointer  hp = make_hazard_pointer();
	EXPECT_EQ(hp.protect(src), &value);
}

TEST(HazardPointer, ArrayAcquiresIndependentHandles) {
	hazard_pointer_array<3> arr;
	EXPECT_EQ(arr.size(), 3u);
	for (std::size_t i = 0; i < arr.size(); ++i) {
		EXPECT_FALSE(arr[i].empty());
	}
}

// --------------------------------------------------------------------------
// Reclamation semantics against an explicit domain
// --------------------------------------------------------------------------

TEST(HazardPointer, ProtectedObjectSurvivesUntilReleased) {
	hazard_pointer_domain domain;
	std::atomic<int>      destroyed{0};

	auto *obj = new Tracked(&destroyed, 1);
	std::atomic<Tracked *> src{obj};

	hazard_pointer hp = make_hazard_pointer(domain);
	Tracked       *p  = hp.protect(src);
	ASSERT_EQ(p, obj);

	src.store(nullptr, std::memory_order_relaxed);
	obj->retire(domain);
	domain.cleanup();
	// Still protected -> must not be reclaimed.
	EXPECT_EQ(destroyed.load(), 0);

	hp.reset_protection();
	domain.cleanup();
	EXPECT_EQ(destroyed.load(), 1);
}

TEST(HazardPointer, UnprotectedObjectIsReclaimed) {
	hazard_pointer_domain domain;
	std::atomic<int>      destroyed{0};

	auto *obj = new Tracked(&destroyed, 1);
	obj->retire(domain);
	domain.cleanup();
	EXPECT_EQ(destroyed.load(), 1);
}

TEST(HazardPointer, DomainDestructorDrainsRetired) {
	std::atomic<int> destroyed{0};
	{
		hazard_pointer_domain domain;
		for (int i = 0; i < 5; ++i) {
			(new Tracked(&destroyed, i))->retire(domain);
		}
		// No cleanup() call: rely on the destructor to drain.
	}
	EXPECT_EQ(destroyed.load(), 5);
}

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

namespace {

// Outcome of one producers+consumers round: every pushed value must be popped
// exactly once. Counters are atomic so workers can update them race-free.
struct StackRunResult {
	std::atomic<int> popped{0};        // total successful pops
	std::atomic<int> doublePops{0};    // a value observed popped more than once
	std::atomic<int> corruptValues{0}; // a popped value outside the valid range
};

// Run one multi-producer / multi-consumer round over a fresh stack. Repeated by
// the caller: the race window is small, so a single pass rarely trips a latent
// bug — many short rounds are far more likely to catch it than one long pass.
void runConcurrentRound(int producers, int consumers, int perProducer,
                        StackRunResult &result) {
	const int          total = producers * perProducer;
	stack<int>         s;
	std::atomic<bool>  producersDone{false};
	std::vector<std::atomic<std::uint8_t>> seen(static_cast<std::size_t>(total));
	for (auto &flag : seen) {
		flag.store(0, std::memory_order_relaxed);
	}

	std::vector<std::thread> threads;
	threads.reserve(static_cast<std::size_t>(producers + consumers));
	for (int p = 0; p < producers; ++p) {
		threads.emplace_back([&, p] {
			for (int i = 0; i < perProducer; ++i) {
				s.push(p * perProducer + i);
			}
		});
	}
	for (int c = 0; c < consumers; ++c) {
		threads.emplace_back([&] {
			while (true) {
				std::optional<int> v = s.pop();
				if (v) {
					if (*v < 0 || *v >= total) {
						result.corruptValues.fetch_add(1,
						                                std::memory_order_relaxed);
						continue;
					}
					// A second observer of the same value means the node was
					// handed out twice — the exact failure hazard pointers exist
					// to prevent.
					if (seen[static_cast<std::size_t>(*v)].fetch_add(
					        1, std::memory_order_relaxed) != 0) {
						result.doublePops.fetch_add(1, std::memory_order_relaxed);
					}
					result.popped.fetch_add(1, std::memory_order_relaxed);
				} else if (producersDone.load(std::memory_order_acquire) &&
				           result.popped.load(std::memory_order_relaxed) >=
				               total) {
					break;
				}
			}
		});
	}

	for (int p = 0; p < producers; ++p) {
		threads[static_cast<std::size_t>(p)].join();
	}
	producersDone.store(true, std::memory_order_release);
	for (int c = producers; c < producers + consumers; ++c) {
		threads[static_cast<std::size_t>(c)].join();
	}
}

} // namespace

TEST(HazardStack, ConcurrentPushPopConservesElements) {
	// Many short rounds rather than one long run: repetition is what turns a
	// rare interleaving into a reproducible failure.
	constexpr int kRounds      = 50;
	constexpr int kProducers   = 4;
	constexpr int kConsumers   = 4;
	constexpr int kPerProducer = 4000;
	constexpr int kTotal       = kProducers * kPerProducer;

	for (int round = 0; round < kRounds; ++round) {
		StackRunResult result;
		runConcurrentRound(kProducers, kConsumers, kPerProducer, result);

		ASSERT_EQ(result.corruptValues.load(), 0)
		    << "round " << round << ": popped an out-of-range value "
		       "(memory corruption / use-after-free)";
		ASSERT_EQ(result.doublePops.load(), 0)
		    << "round " << round << ": a value was popped more than once "
		       "(node reclaimed while still reachable)";
		ASSERT_EQ(result.popped.load(), kTotal)
		    << "round " << round << ": lost elements";
	}
}

// Each thread both pushes and pops, so producers and consumers race on the same
// nodes continuously — the interleaving that most aggressively exercises
// hazard-pointer reclamation (a popped node being retired while another thread
// is mid-traversal of it).
TEST(HazardStack, ConcurrentMixedPushPopIsMemorySafe) {
	constexpr int kThreads = 8;
	constexpr int kOpsEach = 20000;

	stack<int>       s;
	std::atomic<int> corrupt{0};

	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			for (int i = 0; i < kOpsEach; ++i) {
				s.push(t * kOpsEach + i);
				if (std::optional<int> v = s.pop()) {
					// Any pushed value is non-negative; a wild value means we
					// dereferenced freed memory.
					if (*v < 0) {
						corrupt.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}

	EXPECT_EQ(corrupt.load(), 0);
	// The stack must be fully drainable afterwards without crashing.
	while (s.pop().has_value()) {
	}
}

} // namespace
