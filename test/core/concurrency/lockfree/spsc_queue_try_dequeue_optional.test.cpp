#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

using namespace exchange::test::spsc;

namespace {

TEST(SpscQueueTryDequeueOptional, ReturnsNulloptWhenEmpty) {
	spsc_queue<int, 4> q;
	EXPECT_FALSE(q.try_dequeue().has_value());
}

TEST(SpscQueueTryDequeueOptional, DequeuesInFifoOrderThenEmpties) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3}));

	EXPECT_EQ(q.try_dequeue().value_or(-1), 1);
	EXPECT_EQ(q.try_dequeue().value_or(-1), 2);
	EXPECT_EQ(q.try_dequeue().value_or(-1), 3);
	EXPECT_FALSE(q.try_dequeue().has_value());
	EXPECT_TRUE(q.is_empty());
}

} // namespace
