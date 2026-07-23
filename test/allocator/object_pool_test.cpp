#include "memory/object_pool.hpp"

#include <gtest/gtest.h>

#include <unordered_set>
#include <vector>

using memory::ObjectPool;

namespace {

// A small POD payload standing in for a pooled order node. Trivially default
// constructible, as the pool's static_assert requires.
struct Payload {
    std::uint64_t id = 0;
    std::uint64_t token = 0;
};

// --------------------------------------------------------------------------
// Single-threaded semantics
// --------------------------------------------------------------------------

TEST(ObjectPool, ReportsConstructedCapacity) {
    ObjectPool<Payload> pool(64);
    EXPECT_EQ(pool.size(), 64u);
}

TEST(ObjectPool, AllocateReturnsUsableStorage) {
    ObjectPool<Payload> pool(4);
    Payload *p = pool.allocate();
    ASSERT_NE(p, nullptr);
    p->id = 123; // must be writable without faulting
    EXPECT_EQ(p->id, 123u);
    pool.free(p);
}

TEST(ObjectPool, DrainsExactlyCapacityDistinctObjects) {
    // A full drain must hand out `cap` distinct addresses — never the same slot
    // twice while every object is still outstanding.
    constexpr std::uint32_t cap = 256;
    ObjectPool<Payload> pool(cap);

    std::unordered_set<Payload *> seen;
    seen.reserve(cap);
    for (std::uint32_t i = 0; i < cap; ++i) {
        Payload *p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(seen.insert(p).second) << "slot handed out twice at i=" << i;
    }
    for (Payload *p : seen) pool.free(p);
}

TEST(ObjectPool, FreedSlotIsReusedNotLeakedToHeap) {
    // Learn the pool's own address set by draining it once, then verify that
    // after freeing, a fresh allocate comes back from that same set (reuse),
    // rather than falling through to a heap allocation.
    constexpr std::uint32_t cap = 128;
    ObjectPool<Payload> pool(cap);

    std::vector<Payload *> all;
    std::unordered_set<Payload *> pooled;
    for (std::uint32_t i = 0; i < cap; ++i) {
        Payload *p = pool.allocate();
        all.push_back(p);
        pooled.insert(p);
    }
    for (Payload *p : all) pool.free(p);

    // Every allocation of a non-exhausted pool must land back in the pool set.
    for (std::uint32_t i = 0; i < cap; ++i) {
        Payload *p = pool.allocate();
        EXPECT_TRUE(pooled.count(p) == 1)
            << "allocation " << i << " escaped to the heap while slots were free";
    }
    // Drain-and-free bookkeeping balances out; nothing to clean beyond this.
    // (Re-freeing here is unnecessary for the assertion under test.)
}

TEST(ObjectPool, ExhaustionFallsBackToHeapAndStaysUsable) {
    // Past capacity the pool must still return usable, distinct storage (heap
    // fallback), and free() must accept those foreign pointers without crashing.
    constexpr std::uint32_t cap = 16;
    ObjectPool<Payload> pool(cap);

    std::unordered_set<Payload *> unique;
    for (std::uint32_t i = 0; i < cap * 3; ++i) {
        Payload *p = pool.allocate();
        ASSERT_NE(p, nullptr) << "allocate must never return null (heap fallback)";
        p->id = i; // touch heap-fallback storage too
        EXPECT_TRUE(unique.insert(p).second) << "duplicate pointer at i=" << i;
    }
    EXPECT_EQ(unique.size(), cap * 3);
    for (Payload *p : unique) pool.free(p); // pooled return; heap ones delete
}

TEST(ObjectPool, ResetRestoresFullCapacity) {
    constexpr std::uint32_t cap = 32;
    ObjectPool<Payload> pool(cap);

    // Drain fully, then reset instead of freeing.
    for (std::uint32_t i = 0; i < cap; ++i) ASSERT_NE(pool.allocate(), nullptr);
    pool.reset();

    // All slots are available again: another full drain of distinct objects.
    std::unordered_set<Payload *> seen;
    for (std::uint32_t i = 0; i < cap; ++i) {
        Payload *p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(seen.insert(p).second);
    }
}

TEST(ObjectPool, AvailableTracksOutstanding) {
    ObjectPool<Payload> pool(8);
    EXPECT_EQ(pool.available(), 8u);
    Payload *a = pool.allocate();
    Payload *b = pool.allocate();
    EXPECT_EQ(pool.available(), 6u);
    pool.free(a);
    EXPECT_EQ(pool.available(), 7u);
    pool.free(b);
    EXPECT_EQ(pool.available(), 8u);
}

// Note: ObjectPool is single-threaded by contract (one pool per owning thread),
// so there is no concurrency test — sharing a pool across threads is a usage
// error, not a case to verify.

} // namespace
