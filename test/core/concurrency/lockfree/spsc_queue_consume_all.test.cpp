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

// Only the return value is checkable here. consume_all also declines to
// republish an unchanged read cursor on an empty queue, which matters - the
// cursor is on the consumer's cache line and the producer loads it in has_room,
// so a store invalidates the other core for no state change, once per poll of
// an empty queue. That is invisible from out here: no observer reports the
// cursor, and size() reads the same either way. It is pinned by the argument in
// the header rather than by this suite.
TEST(SpscQueueConsumeAll, EmptyQueueReturnsZeroAndStaysUsable) {
	spsc_queue<int, 8> q;

	EXPECT_EQ(q.consume_all([](int &) noexcept {}), 0u);
	EXPECT_TRUE(q.is_empty());

	// The early return must not have stranded the cursors against each other.
	ASSERT_TRUE(q.try_emplace(7));
	int seen = 0;
	EXPECT_EQ(q.consume_all([&](int &v) noexcept { seen = v; }), 1u);
	EXPECT_EQ(seen, 7);
	EXPECT_TRUE(q.is_empty());
}

} // namespace
