#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

#include "SPSCQueue.hpp"

namespace {

/// @brief Drain the queue into a vector, preserving FIFO order.
template<class T, size_t N>
std::vector<T> drain(SPSCQueue<T, N> &q) {
    std::vector<T> out;
    while (auto v = q.try_pop()) { out.emplace_back(*v); }
    return out;
}

} // namespace

// --------------------------------------------------------------------------
// try_emplace_range — happy paths
// --------------------------------------------------------------------------

TEST(SpscQueueTryEmplaceRange, EnqueuesWholeRangeInOrder) {
    SPSCQueue<int, 8> q;
    const std::array<int, 4> src{1, 2, 3, 4};

    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(drain(q), (std::vector<int>{1, 2, 3, 4}));
}

TEST(SpscQueueTryEmplaceRange, FillsExactlyToCapacity) {
    // Effective capacity is N (one slot reserved to distinguish full/empty).
    SPSCQueue<int, 4> q;
    const std::array<int, 4> src{10, 20, 30, 40};

    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(drain(q), (std::vector<int>{10, 20, 30, 40}));
}

TEST(SpscQueueTryEmplaceRange, EmptyRangeSucceedsAndIsANoOp) {
    SPSCQueue<int, 4> q;
    const std::array<int, 0> empty{};
    ASSERT_TRUE(q.try_emplace_range(empty));
    EXPECT_FALSE(q.try_pop().has_value());
}

TEST(SpscQueueTryEmplaceRange, InterleavesWithSingleEmplace) {
    SPSCQueue<int, 8> q;
    ASSERT_TRUE(q.try_emplace(7));
    const std::array<int, 2> src{8, 9};
    ASSERT_TRUE(q.try_emplace_range(src));

    EXPECT_EQ(drain(q), (std::vector<int>{7, 8, 9}));
}

TEST(SpscQueueTryEmplaceRange, AcceptsAVectorRange) {
    SPSCQueue<int, 8> q;
    const std::vector<int> src{1, 2, 3};
    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(drain(q), (std::vector<int>{1, 2, 3}));
}

// --------------------------------------------------------------------------
// try_emplace_range — wrap-around
// --------------------------------------------------------------------------

TEST(SpscQueueTryEmplaceRange, RangeStraddlingWrapBoundaryIsReassembled) {
    SPSCQueue<int, 4> q;

    // Advance the write cursor near the end of the backing array, then drain
    // so a subsequent range must wrap around the physical buffer end.
    const std::array<int, 3> warmup{1, 2, 3};
    ASSERT_TRUE(q.try_emplace_range(warmup));
    EXPECT_EQ(drain(q), (std::vector<int>{1, 2, 3}));

    const std::array<int, 4> src{4, 5, 6, 7};
    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(drain(q), (std::vector<int>{4, 5, 6, 7}));
}

TEST(SpscQueueTryEmplaceRange, RepeatedWrapKeepsFifoOrder) {
    SPSCQueue<int, 4> q;
    int next = 0;
    for (int iter = 0; iter < 100; ++iter) {
        const std::array<int, 3> src{next, next + 1, next + 2};
        ASSERT_TRUE(q.try_emplace_range(src)) << "iteration " << iter;
        EXPECT_EQ(drain(q), (std::vector<int>{next, next + 1, next + 2}))
                << "iteration " << iter;
        next += 3;
    }
}

// --------------------------------------------------------------------------
// try_emplace_range — unhappy paths (reservation is all-or-nothing)
// --------------------------------------------------------------------------

TEST(SpscQueueTryEmplaceRange, RejectsRangeLargerThanCapacity) {
    SPSCQueue<int, 4> q;
    const std::array<int, 5> src{1, 2, 3, 4, 5}; // one past effective capacity

    EXPECT_FALSE(q.try_emplace_range(src));
    EXPECT_FALSE(q.try_pop().has_value()); // untouched on failure
}

TEST(SpscQueueTryEmplaceRange, RejectsWhenPartiallyFull) {
    SPSCQueue<int, 4> q;
    ASSERT_TRUE(q.try_emplace(1));
    ASSERT_TRUE(q.try_emplace(2)); // 2 free slots remain

    const std::array<int, 3> src{3, 4, 5};
    EXPECT_FALSE(q.try_emplace_range(src));
    // Existing contents preserved, nothing from the rejected range leaks in.
    EXPECT_EQ(drain(q), (std::vector<int>{1, 2}));
}

TEST(SpscQueueTryEmplaceRange, SucceedsAgainAfterDrainingFreesSpace) {
    SPSCQueue<int, 4> q;
    const std::array<int, 4> full{1, 2, 3, 4};
    ASSERT_TRUE(q.try_emplace_range(full));

    const std::array<int, 2> more{5, 6};
    EXPECT_FALSE(q.try_emplace_range(more)); // full

    ASSERT_EQ(q.try_pop().value_or(-1), 1);
    ASSERT_EQ(q.try_pop().value_or(-1), 2);
    EXPECT_TRUE(q.try_emplace_range(more)); // room now
    EXPECT_EQ(drain(q), (std::vector<int>{3, 4, 5, 6}));
}

// --------------------------------------------------------------------------
// Observers: is_empty() / size()
// --------------------------------------------------------------------------

TEST(SpscQueueObservers, FreshQueueIsEmptyWithZeroSize) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(SpscQueueObservers, SizeTracksEmplaceAndPop) {
    SPSCQueue<int, 8> q;
    ASSERT_TRUE(q.try_emplace(1));
    ASSERT_TRUE(q.try_emplace(2));
    EXPECT_FALSE(q.is_empty());
    EXPECT_EQ(q.size(), 2u);

    const std::array<int, 3> src{3, 4, 5};
    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(q.size(), 5u);

    ASSERT_TRUE(q.try_pop().has_value());
    EXPECT_EQ(q.size(), 4u);
}

TEST(SpscQueueObservers, SizeIsCorrectAcrossWrapBoundary) {
    SPSCQueue<int, 4> q;
    const std::array<int, 3> warmup{1, 2, 3};
    ASSERT_TRUE(q.try_emplace_range(warmup));
    ASSERT_EQ(drain(q).size(), 3u); // advance cursors toward the wrap point

    const std::array<int, 4> src{4, 5, 6, 7}; // this batch wraps
    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(q.size(), 4u);
    EXPECT_FALSE(q.is_empty());
}

// --------------------------------------------------------------------------
// clear()
// --------------------------------------------------------------------------

TEST(SpscQueueClear, DropsAllPendingElements) {
    SPSCQueue<int, 8> q;
    const std::array<int, 4> src{1, 2, 3, 4};
    ASSERT_TRUE(q.try_emplace_range(src));
    ASSERT_EQ(q.size(), 4u);

    q.clear();
    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_FALSE(q.try_pop().has_value());
}

TEST(SpscQueueClear, QueueIsReusableAfterClear) {
    SPSCQueue<int, 4> q;
    ASSERT_TRUE(q.try_emplace(1));
    ASSERT_TRUE(q.try_emplace(2));
    q.clear();

    const std::array<int, 4> src{5, 6, 7, 8};
    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(drain(q), (std::vector<int>{5, 6, 7, 8}));
}

TEST(SpscQueueClear, ClearingAnEmptyQueueIsANoOp) {
    SPSCQueue<int, 4> q;
    q.clear();
    EXPECT_TRUE(q.is_empty());
    EXPECT_EQ(q.size(), 0u);
}
