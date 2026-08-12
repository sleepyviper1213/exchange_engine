#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>


namespace {

TEST(SpscQueueTryDequeueOutParam, ReturnsFalseAndLeavesOutUntouchedWhenEmpty) {
	spsc_queue<int, 4> q;
	int v = 42;
	EXPECT_FALSE(q.try_dequeue(v));
	EXPECT_EQ(v, 42);
}

TEST(SpscQueueTryDequeueOutParam, KeepsFifoOrderAcrossWrapBoundary) {
	spsc_queue<int, 4> q;
	int next = 0;
	for (int iter = 0; iter < 100; ++iter) {
		const std::array<int, 3> src{next, next + 1, next + 2};
		ASSERT_TRUE(q.try_emplace_range(src)) << "iteration " << iter;

		int a = 0;
		int b = 0;
		int c = 0;
		ASSERT_TRUE(q.try_dequeue(a)) << "iteration " << iter;
		ASSERT_TRUE(q.try_dequeue(b)) << "iteration " << iter;
		ASSERT_TRUE(q.try_dequeue(c)) << "iteration " << iter;
		EXPECT_EQ(a, next);
		EXPECT_EQ(b, next + 1);
		EXPECT_EQ(c, next + 2);
		next += 3;
	}
}

} // namespace
