#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

using namespace exchange::test::spsc;

namespace {

TEST(SpscQueueTryPopOutParam, PopsElementsInFifoOrder) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	ASSERT_TRUE(q.try_emplace(3));

	int v = 0;
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 1);
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 2);
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 3);
	EXPECT_FALSE(q.try_dequeue(v));
	EXPECT_TRUE(q.is_empty());
}

} // namespace
