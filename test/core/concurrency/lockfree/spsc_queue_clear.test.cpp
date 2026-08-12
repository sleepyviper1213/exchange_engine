#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>


namespace {

TEST(SpscQueueClear, DropsAllPendingElements) {
	spsc_queue<int, 8> q;
	const std::array<int, 4> src{1, 2, 3, 4};
	ASSERT_TRUE(q.try_emplace_range(src));
	ASSERT_EQ(q.size(), 4u);

	q.clear();
	EXPECT_TRUE(q.is_empty());
	EXPECT_FALSE(q.try_dequeue().has_value());
}

TEST(SpscQueueClear, QueueIsReusableAfterClear) {
	spsc_queue<int, 4> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	q.clear();

	const std::array<int, 4> src{5, 6, 7, 8};
	ASSERT_TRUE(q.try_emplace_range(src));
	EXPECT_EQ(drain(q), (std::vector<int>{5, 6, 7, 8}));
}

TEST(SpscQueueClear, ClearingAnEmptyQueueIsANoOp) {
	spsc_queue<int, 4> q;
	q.clear();
	EXPECT_TRUE(q.is_empty());
}

} // namespace
