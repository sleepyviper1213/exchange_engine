#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

using namespace exchange::test::spsc;

namespace {

TEST(SpscQueueTryEmplace, FillsToCapacityThenRejectsWhenFull) {
	spsc_queue<int, 4> q;
	for (int i = 0; i < 4; ++i)
		ASSERT_TRUE(q.try_emplace(i)) << "slot " << i;

	EXPECT_TRUE(q.is_full());
	EXPECT_FALSE(q.try_emplace(99));
	EXPECT_EQ(q.size(), 4u);
	// The rejected value never entered the ring.
	EXPECT_EQ(drain(q), (std::vector{0, 1, 2, 3}));
}

TEST(SpscQueueTryEmplace, KeepsFifoOrder) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	ASSERT_TRUE(q.try_emplace(3));

	EXPECT_EQ(drain(q), (std::vector{1, 2, 3}));
}

TEST(SpscQueueTryEmplace, AcceptsAgainAfterDequeueFreesASlot) {
	spsc_queue<int, 2> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	ASSERT_TRUE(q.is_full());
	EXPECT_FALSE(q.try_emplace(3));

	int v = 0;
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 1);
	EXPECT_TRUE(q.try_emplace(3));

	EXPECT_EQ(drain(q), (std::vector{2, 3}));
}

TEST(SpscQueueTryEmplace, ForwardsMultipleConstructorArguments) {
	spsc_queue<std::pair<int, int>, 4> q;
	ASSERT_TRUE(q.try_emplace(1, 2));

	const auto v = q.try_dequeue();
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(v->first, 1);
	EXPECT_EQ(v->second, 2);
}

} // namespace
