#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>


namespace {

TEST(SpscQueueObservers, FreshQueueIsEmptyWithZeroSize) {
	spsc_queue<int, 4> q;
	EXPECT_TRUE(q.is_empty());
}

TEST(SpscQueueObservers, SizeTracksEmplaceAndDequeue) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	EXPECT_FALSE(q.is_empty());
	EXPECT_EQ(q.size(), 2u);

	const std::array<int, 3> src{3, 4, 5};
	ASSERT_TRUE(q.try_emplace_range(src));
	EXPECT_EQ(q.size(), 5u);

	ASSERT_TRUE(q.try_dequeue().has_value());
	EXPECT_EQ(q.size(), 4u);
}

TEST(SpscQueueObservers, SizeIsCorrectAcrossWrapBoundary) {
	spsc_queue<int, 4> q;
	const std::array<int, 3> warmup{1, 2, 3};
	ASSERT_TRUE(q.try_emplace_range(warmup));
	ASSERT_EQ(drain(q).size(), 3u); // advance cursors toward the wrap point

	const std::array<int, 4> src{4, 5, 6, 7}; // this batch wraps
	ASSERT_TRUE(q.try_emplace_range(src));
	EXPECT_EQ(q.size(), 4u);
	EXPECT_FALSE(q.is_empty());
}

TEST(SpscQueueObservers, IsFullOnlyWhenAllSlotsTaken) {
	spsc_queue<int, 4> q;
	EXPECT_FALSE(q.is_full());

	for (int i = 0; i < 4; ++i)
		ASSERT_TRUE(q.try_emplace(i));
	EXPECT_TRUE(q.is_full());

	int v = 0;
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_FALSE(q.is_full());
}

} // namespace
