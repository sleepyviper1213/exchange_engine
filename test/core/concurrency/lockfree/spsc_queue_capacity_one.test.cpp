#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

using namespace exchange::test::spsc;

namespace {

TEST(SpscQueueCapacityOne, AlternatesBetweenEmptyAndFull) {
	spsc_queue<int, 1> q;
	EXPECT_TRUE(q.is_empty());
	EXPECT_FALSE(q.is_full());

	ASSERT_TRUE(q.try_emplace(1));
	EXPECT_TRUE(q.is_full());
	EXPECT_EQ(q.size(), 1u);
	EXPECT_FALSE(q.try_emplace(2)); // there is no second slot

	int v = 0;
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 1);
	EXPECT_TRUE(q.is_empty());
	EXPECT_FALSE(q.try_dequeue(v));

	// Every push reuses slot 0, so this laps the ring repeatedly.
	for (int i = 0; i < 10; ++i) {
		ASSERT_TRUE(q.try_emplace(i)) << "iteration " << i;
		ASSERT_TRUE(q.try_dequeue(v)) << "iteration " << i;
		EXPECT_EQ(v, i);
	}
}

TEST(SpscQueueCapacityOne, BatchPathsRespectTheSingleSlot) {
	spsc_queue<int, 1> q;

	const std::array<int, 2> two{1, 2};
	EXPECT_FALSE(q.try_emplace_range(two)); // one past capacity
	EXPECT_TRUE(q.is_empty());

	const std::array<int, 1> one{7};
	ASSERT_TRUE(q.try_emplace_range(one));
	EXPECT_TRUE(q.is_full());

	std::array<int, 4> out{};
	EXPECT_EQ(q.try_dequeue_range(out), 1u); // capped by what is available
	EXPECT_EQ(out[0], 7);
	EXPECT_TRUE(q.is_empty());
}

} // namespace
