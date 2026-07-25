#include "memory/detail/freelist/pool.hpp"
#include "util/counted.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

using memory::pool::free_list;
using util::counted;

// --------------------------------------------------------------------------
// Single-threaded correctness
// --------------------------------------------------------------------------

TEST(PoolFreeList, AcquireConstructsWithForwardedArgs) {
	free_list<counted> pool(4);
	const int base = counted::alive.load();

	counted *c = pool.acquire(42);
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(c->value, 42);
	EXPECT_EQ(counted::alive.load(), base + 1);

	pool.release(c);
	EXPECT_EQ(counted::alive.load(), base); // destroyed on release
}

TEST(PoolFreeList, AcquireReturnsNullAtCapacity) {
	free_list<int> pool(3);
	int *a = pool.acquire(1);
	int *b = pool.acquire(2);
	int *c = pool.acquire(3);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_NE(c, nullptr);

	EXPECT_EQ(pool.acquire(4), nullptr); // no free node left

	pool.release(a);
	pool.release(b);
	pool.release(c);
}

TEST(PoolFreeList, AcquireReturnsDistinctStorage) {
	free_list<int> pool(3);
	int *a = pool.acquire(1);
	int *b = pool.acquire(2);
	int *c = pool.acquire(3);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_NE(c, nullptr);
	// A pool that handed the same node out twice would alias here.
	EXPECT_NE(a, b);
	EXPECT_NE(b, c);
	EXPECT_NE(a, c);
	*a = 10;
	*b = 20;
	*c = 30;
	EXPECT_EQ(*a, 10);
	EXPECT_EQ(*b, 20);
	EXPECT_EQ(*c, 30);

	pool.release(a);
	pool.release(b);
	pool.release(c);
}

TEST(PoolFreeList, QuiesceMakesReleasedNodesAvailable) {
	free_list<int> pool(2);
	int *a = pool.acquire(1);
	int *b = pool.acquire(2);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	pool.release(a);
	pool.release(b);

	// Reclamation is batched, so the released nodes may still be draining back;
	// quiesce() forces them home so the full capacity is acquirable again.
	pool.quiesce();
	int *c = pool.acquire(3);
	int *d = pool.acquire(4);
	EXPECT_NE(c, nullptr);
	EXPECT_NE(d, nullptr);

	pool.release(c);
	pool.release(d);
}

TEST(PoolFreeList, ReleaseThenQuiesceReusesNodes) {
	// Over many acquire/release/quiesce cycles a capacity-1 pool must keep
	// working: the single node is retired and reclaimed round after round.
	free_list<int> pool(1);
	for (int i = 0; i < 100; ++i) {
		int *p = pool.acquire(i);
		ASSERT_NE(p, nullptr) << "iteration " << i;
		EXPECT_EQ(*p, i);
		pool.release(p);
		pool.quiesce();
	}
}

TEST(PoolFreeList, DestructorDestroysReleasedElements) {
	const int base = counted::alive.load();
	{
		free_list<counted> pool(8);
		// Acquire several, release some, leave the rest to the destructor's
		// reclamation path. Released elements are already destroyed; the nodes
		// (not live Ts) are what the destructor frees.
		counted *a = pool.acquire(1);
		counted *b = pool.acquire(2);
		counted *c = pool.acquire(3);
		ASSERT_NE(a, nullptr);
		ASSERT_NE(b, nullptr);
		ASSERT_NE(c, nullptr);
		pool.release(b);
		pool.release(c);
		pool.release(a);
	}
	EXPECT_EQ(counted::alive.load(), base); // all balanced
}

// --------------------------------------------------------------------------
// Concurrency: designed to expose reclamation/ABA defects, not throughput.
//
// The failure this pool exists to prevent is handing the same physical node to
// two threads at once — the symptom of an ABA-corrupted CAS or a node reclaimed
// while still in flight. Each worker stamps a unique nonce into its acquired
// element, spins, and checks the stamp is untouched; a double hand-out lets a
// second worker overwrite the storage and the stamp check fails. gtest macros
// are not thread-safe, so workers record anomalies into atomics and the main
// thread asserts.
// --------------------------------------------------------------------------

namespace {
struct Cell {
	std::atomic<std::uint64_t> stamp{0};
};
} // namespace

TEST(PoolFreeList, ConcurrentAcquireReleaseNeverReturnsNodeTwice) {
	constexpr int kThreads     = 8;
	constexpr int kOpsEach     = 40000;
	constexpr std::size_t kCap = 16;
	// deliberately smaller than kThreads * live

	free_list<Cell> pool(kCap);
	std::atomic<std::uint64_t> corruption{0};
	std::atomic<int> completed{0};

	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			// Per-thread, per-iteration unique nonce (thread id in the high
			// bits).
			std::uint64_t nonce = static_cast<std::uint64_t>(t + 1) << 40;
			for (int i = 0; i < kOpsEach; ++i) {
				Cell *cell = pool.acquire();
				if (cell == nullptr) continue;
				// pool momentarily drained; retry
				const std::uint64_t mine = ++nonce;
				cell->stamp.store(mine, std::memory_order_relaxed);
				// Small window in which a double hand-out would let another
				// thread stamp the same storage.
				for (int spin = 0; spin < 4; ++spin)
					std::atomic_signal_fence(std::memory_order_seq_cst);
				if (cell->stamp.load(std::memory_order_relaxed) != mine)
					corruption.fetch_add(1, std::memory_order_relaxed);
				pool.release(cell);
			}
			completed.fetch_add(1, std::memory_order_relaxed);
		});
	}
	for (auto &th : threads) th.join();

	EXPECT_EQ(corruption.load(), 0u)
		<< "a node was handed to two threads at once (ABA / double reclaim)";
	EXPECT_EQ(completed.load(), kThreads);

	// The pool must still be fully usable: after draining pending reclamations,
	// exactly kCap nodes are acquirable, no more and no fewer.
	pool.quiesce();
	std::vector<Cell *> got;
	while (Cell *c = pool.acquire()) got.push_back(c);
	EXPECT_EQ(got.size(), kCap)
		<< "pool lost or duplicated nodes under contention";
	// Distinctness: no physical node appears twice in a full drain.
	std::unordered_set<Cell *> unique(got.begin(), got.end());
	EXPECT_EQ(unique.size(), got.size());
	for (Cell *c : got) pool.release(c);
}
