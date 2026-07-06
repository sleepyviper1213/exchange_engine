#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>

#include "spsc_queue.hpp"

// Non-POD element support for SPSCQueue: elements are constructed in-place on
// push and destroyed on pop, so lifetime must balance exactly (no leaks, no
// double-destroy), the destructor and clear() must reclaim unconsumed elements,
// and move-only types must round-trip. Trivially copyable behaviour is covered
// by spsc_queue_test.cpp.

namespace {
    /// @brief Instance-counting element used to detect leaks and double frees.
    /// Nothrow-move-constructible, as SPSCQueue requires.
    struct Counted {
        static inline int alive = 0;
        int value = 0;

        explicit Counted(int v = 0) noexcept : value(v) { ++alive; }
        Counted(const Counted &o) noexcept : value(o.value) { ++alive; }

        Counted(Counted &&o) noexcept : value(o.value) {
            o.value = -1;
            ++alive;
        }

        Counted &operator=(const Counted &o) noexcept {
            value = o.value;
            return *this;
        }

        Counted &operator=(Counted &&o) noexcept {
            value = o.value;
            o.value = -1;
            return *this;
        }

        ~Counted() { --alive; }
    };
} // namespace

// --------------------------------------------------------------------------
// Lifetime: construction/destruction must balance
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, PushPopPreservesValueAndBalancesLifetime) {
    const int base = Counted::alive;
    {
        spsc_queue<Counted, 4> q;
        ASSERT_TRUE(q.try_emplace(7));
        EXPECT_EQ(q.size(), 1u);

        auto v = q.try_pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(v->value, 7);
        EXPECT_TRUE(q.is_empty());
    }
    EXPECT_EQ(Counted::alive, base); // no leak, no double-destroy
}

TEST(SpscQueueNonPod, DestructorDestroysUnconsumedElements) {
    const int base = Counted::alive;
    {
        spsc_queue<Counted, 8> q;
        ASSERT_TRUE(q.try_emplace(1));
        ASSERT_TRUE(q.try_emplace(2));
        ASSERT_TRUE(q.try_emplace(3));
        EXPECT_EQ(Counted::alive, base + 3);
        // Deliberately leave all three enqueued: the destructor must clean up.
    }
    EXPECT_EQ(Counted::alive, base);
}

TEST(SpscQueueNonPod, ClearDestroysPendingAndQueueStaysUsable) {
    const int base = Counted::alive;
    spsc_queue<Counted, 4> q;
    ASSERT_TRUE(q.try_emplace(1));
    ASSERT_TRUE(q.try_emplace(2));
    EXPECT_EQ(Counted::alive, base + 2);

    q.clear();
    EXPECT_EQ(Counted::alive, base); // pending elements destroyed
    EXPECT_TRUE(q.is_empty());

    ASSERT_TRUE(q.try_emplace(9)); // reusable after clear
    const auto v = q.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->value, 9);
}

TEST(SpscQueueNonPod, RejectsWhenFullWithoutConstructing) {
    const int base = Counted::alive;
    spsc_queue<Counted, 2> q;
    ASSERT_TRUE(q.try_emplace(1));
    ASSERT_TRUE(q.try_emplace(2)); // at capacity (N == 2)
    EXPECT_EQ(Counted::alive, base + 2);

    EXPECT_FALSE(q.try_emplace(3));      // rejected
    EXPECT_EQ(Counted::alive, base + 2); // nothing constructed on failure
}

// --------------------------------------------------------------------------
// FIFO order with a non-trivial type, exercising the wrap boundary
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, KeepsFifoOrderAcrossWrap) {
    spsc_queue<Counted, 4> q;
    int next = 0;
    for (int iter = 0; iter < 50; ++iter) {
        ASSERT_TRUE(q.try_emplace(next)) << "iteration " << iter;
        ASSERT_TRUE(q.try_emplace(next + 1)) << "iteration " << iter;

        Counted a{-1};
        Counted b{-1};
        ASSERT_TRUE(q.try_pop(a)) << "iteration " << iter;
        ASSERT_TRUE(q.try_pop(b)) << "iteration " << iter;
        EXPECT_EQ(a.value, next);
        EXPECT_EQ(b.value, next + 1);
        next += 2;
    }
}

// --------------------------------------------------------------------------
// Move-only element type
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, SupportsMoveOnlyTypeViaOptionalPop) {
    spsc_queue<std::unique_ptr<int>, 4> q;
    ASSERT_TRUE(q.try_emplace(std::make_unique<int>(42)));

    auto popped = q.try_pop();
    ASSERT_TRUE(popped.has_value());
    ASSERT_NE(*popped, nullptr);
    EXPECT_EQ(**popped, 42);
}

TEST(SpscQueueNonPod, SupportsMoveOnlyTypeViaOutParamPop) {
    spsc_queue<std::unique_ptr<int>, 4> q;
    ASSERT_TRUE(q.try_emplace(std::make_unique<int>(7)));

    std::unique_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
}

// --------------------------------------------------------------------------
// try_emplace_range with a non-trivial (copy) element type
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, EmplaceRangeCopiesNonTrivialElements) {
    spsc_queue<std::string, 8> q;
    const std::array<std::string, 3> src{"alpha", "beta", "gamma"};

    ASSERT_TRUE(q.try_emplace_range(src));
    EXPECT_EQ(src[0], "alpha"); // copied, source untouched

    std::string out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, "alpha");
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, "beta");
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, "gamma");
    EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscQueueNonPod, EmplaceRangeConstructsAcrossWrapInOrder) {
    spsc_queue<std::string, 4> q;

    // Advance the write cursor near the physical end, then drain, so the next
    // range must wrap around the buffer end in the element-wise construct path.
    const std::array<std::string, 3> warmup{"a", "b", "c"};
    ASSERT_TRUE(q.try_emplace_range(warmup));
    std::string sink;
    for (int i = 0; i < 3; ++i) { ASSERT_TRUE(q.try_pop(sink)); }

    const std::array<std::string, 4> src{"w", "x", "y", "z"};
    ASSERT_TRUE(q.try_emplace_range(src));

    std::string out;
    for (const auto &expected: src) {
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, expected);
    }
}

TEST(SpscQueueNonPod, PopFromEmptyLeavesOutParamUntouched) {
    spsc_queue<std::string, 4> q;
    EXPECT_FALSE(q.try_pop().has_value());

    std::string out = "keep";
    EXPECT_FALSE(q.try_pop(out));
    EXPECT_EQ(out, "keep");
}
