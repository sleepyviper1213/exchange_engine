#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>


namespace {

TEST(SpscQueueConsumeUpTo, ConsumesRequestedCount) {
	spsc_queue<int, 8> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3, 4}));

	const auto n = q.consume_up_to(2, [](int &) noexcept {});

	EXPECT_EQ(n, 2u);

	EXPECT_EQ(q.size(), 2u);

	// The front two were consumed, so only the tail remains, in order.
	EXPECT_EQ(drain(q), (std::vector{3, 4}));
}

TEST(SpscQueueConsumeUpTo, ConsumesRemainingElements) {
	spsc_queue<int, 8> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2}));

	// The lockfree empties, so record the delivered values through a nothrow
	// accumulator rather than draining afterwards.
	long consumed_sum = 0;

	EXPECT_EQ(q.consume_up_to(8, [&](int &v) noexcept { consumed_sum += v; }),
	          2u);

	EXPECT_TRUE(q.is_empty());

	EXPECT_EQ(consumed_sum, 1 + 2);
}

TEST(SpscQueueConsumeUpTo, EmptyQueueReturnsZero) {
	spsc_queue<int32_t, 8> q;

	EXPECT_EQ(q.consume_up_to(8, [](int &) noexcept {}), 0u);
}

TEST(SpscQueueConsumeUpTo, ZeroLimitConsumesNothing) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3}));

	EXPECT_EQ(q.consume_up_to(0, [](int &) noexcept {}), 0u);
	EXPECT_EQ(q.size(), 3u);
	EXPECT_EQ(drain(q), (std::vector{1, 2, 3}));
}

} // namespace
