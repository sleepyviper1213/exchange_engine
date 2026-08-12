#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>


namespace {

TEST(SpscQueueConsumeAll, ConsumesEverything) {
	spsc_queue<int, 8> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3, 4}));

	long consumed_sum = 0;

	const auto n = q.consume_all([&](int &v) noexcept { consumed_sum += v; });

	EXPECT_EQ(n, 4u);

	EXPECT_TRUE(q.is_empty());

	EXPECT_EQ(consumed_sum, 1 + 2 + 3 + 4);
}

TEST(SpscQueueConsumeAll, EmptyQueueReturnsZero) {
	spsc_queue<int, 8> q;

	EXPECT_EQ(q.consume_all([](int &) noexcept {}), 0u);
}

} // namespace
