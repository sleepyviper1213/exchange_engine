#include "core/memory/object_pool.hpp"

#include <gtest/gtest.h>

#include <unordered_set>
#include <vector>

using exchange::core::memory::object_pool;

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
    object_pool<Payload> pool(64);
    EXPECT_EQ(pool.size(), 64u);
}

TEST(ObjectPool, AllocateReturnsUsableStorage) {
    object_pool<Payload> pool(4);
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
    object_pool<Payload> pool(cap);

    std::unordered_set<Payload *> seen;
    seen.reserve(cap);
    for (std::uint32_t i = 0; i < cap; ++i) {
        Payload *p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(seen.insert(p).second) << "slot handed out twice at i=" << i;
    }
    for (Payload *p : seen) pool.free(p);
}

TEST(ObjectPool, FreedSlotIsReused) {
    // Learn the pool's own address set by draining it once, then verify that
    // after freeing, a fresh allocate comes back from that same set — a cell is
    // recycled, not stranded, so capacity is genuinely reusable rather than
    // consumed once.
    constexpr std::uint32_t cap = 128;
    object_pool<Payload> pool(cap);

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
            << "allocation " << i << " left the pool's cells while slots were free";
    }
    // Drain-and-free bookkeeping balances out; nothing to clean beyond this.
    // (Re-freeing here is unnecessary for the assertion under test.)
}

TEST(ObjectPool, ExhaustionReturnsNullRatherThanAllocating) {
    // The pool is fixed size: past capacity it reports exhaustion instead of
    // reaching for the allocator. A silent heap fallback would keep the caller
    // working while injecting exactly the allocation jitter the pool exists to
    // remove, so the null is the contract, not a failure mode.
    constexpr std::uint32_t cap = 16;
    object_pool<Payload> pool(cap);

    std::vector<Payload *> held;
    for (std::uint32_t i = 0; i < cap; ++i) {
        Payload *p = pool.allocate();
        ASSERT_NE(p, nullptr) << "pool must serve its full capacity";
        held.push_back(p);
    }
    EXPECT_TRUE(pool.exhausted());
    EXPECT_EQ(pool.allocate(), nullptr);
    EXPECT_EQ(pool.allocate(), nullptr) << "exhaustion must be repeatable";

    // Freeing one makes exactly one more allocation succeed.
    pool.free(held.back());
    held.pop_back();
    EXPECT_FALSE(pool.exhausted());
    Payload *reused = pool.allocate();
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(pool.allocate(), nullptr);

    held.push_back(reused);
    for (Payload *p : held) pool.free(p);
    EXPECT_EQ(pool.available(), cap);
}

TEST(ObjectPool, FreeingNullIsANoOp) {
    // allocate() can return null, so handing that result straight back must be
    // harmless — otherwise every caller needs a branch the pool could own.
    object_pool<Payload> pool(2);
    pool.free(nullptr);
    EXPECT_EQ(pool.available(), 2u);
}

TEST(ObjectPool, ResetRestoresFullCapacity) {
    constexpr std::uint32_t cap = 32;
    object_pool<Payload> pool(cap);

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
    object_pool<Payload> pool(8);
    EXPECT_EQ(pool.available(), 8u);
    Payload *a = pool.allocate();
    Payload *b = pool.allocate();
    EXPECT_EQ(pool.available(), 6u);
    pool.free(a);
    EXPECT_EQ(pool.available(), 7u);
    pool.free(b);
    EXPECT_EQ(pool.available(), 8u);
}

// Note: object_pool is single-threaded by contract (one pool per owning thread),
// so there is no concurrency test — sharing a pool across threads is a usage
// error, not a case to verify.

} // namespace
