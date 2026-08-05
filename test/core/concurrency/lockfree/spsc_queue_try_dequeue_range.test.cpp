#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

using namespace exchange::test::spsc;

namespace {

TEST(SpscQueueTryDequeueRange, DequeuesWholeBatch) {
	spsc_queue<int, 8> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3, 4}));

	std::array<int, 4> out{};

	EXPECT_EQ(q.try_dequeue_range(out), 4u);

	EXPECT_EQ(out, (std::array{1, 2, 3, 4}));

	EXPECT_TRUE(q.is_empty());
}

TEST(SpscQueueTryDequeueRange, DequeuesOnlyAvailableElements) {
	spsc_queue<int, 8> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2}));

	std::array<int, 4> out{};

	EXPECT_EQ(q.try_dequeue_range(out), 2u);

	EXPECT_EQ(out[0], 1);
	EXPECT_EQ(out[1], 2);

	EXPECT_TRUE(q.is_empty());
}

TEST(SpscQueueTryDequeueRange, EmptyQueueReturnsZero) {
	spsc_queue<int, 8> q;

	std::array<int, 8> out{};

	EXPECT_EQ(q.try_dequeue_range(out), 0u);
}

TEST(SpscQueueTryDequeueRange, HandlesWrapAround) {
	spsc_queue<int, 4> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3}));

	EXPECT_EQ(drain(q), (std::vector{1, 2, 3}));

	ASSERT_TRUE(q.try_emplace_range(std::array{4, 5, 6, 7}));

	std::array<int, 4> out{};

	EXPECT_EQ(q.try_dequeue_range(out), 4u);

	EXPECT_EQ(out, (std::array{4, 5, 6, 7}));
}

TEST(SpscQueueTryDequeueRange, CapsAtBufferSizeWhenMoreAvailable) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3, 4}));

	std::array<int, 2> out{};
	EXPECT_EQ(q.try_dequeue_range(out), 2u);
	EXPECT_EQ(out, (std::array{1, 2}));
	EXPECT_EQ(q.size(), 2u);
	EXPECT_EQ(drain(q), (std::vector{3, 4}));
}

TEST(SpscQueueTryDequeueRange, EmptyOutputRangeDequeuesNothing) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3}));

	// The buffer size caps the batch, so a zero-length one is a no-op even
	// though elements are available.
	std::array<int, 0> out{};
	EXPECT_EQ(q.try_dequeue_range(out), 0u);

	EXPECT_EQ(q.size(), 3u);
	EXPECT_EQ(drain(q), (std::vector{1, 2, 3}));
}

} // namespace
