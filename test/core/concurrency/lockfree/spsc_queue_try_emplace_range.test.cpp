#include "spsc_queue.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>


namespace {

TEST(SpscQueueTryEmplaceRange, EnqueuesWholeRangeInOrder) {
	spsc_queue<int, 8> q;
	const std::array<int, 4> src{1, 2, 3, 4};

	ASSERT_TRUE(q.try_emplace_range(src));
	EXPECT_EQ(drain(q), (std::vector<int>{1, 2, 3, 4}));
}

TEST(SpscQueueTryEmplaceRange, FillsExactlyToCapacity) {
	// All N slots are usable: absolute cursors make full (write-read==N) and
	// empty (write==read) distinguishable, so no sentinel slot is reserved.
	spsc_queue<int, 4> q;
	const std::array<int, 4> src{10, 20, 30, 40};

	ASSERT_TRUE(q.try_emplace_range(src));
	ASSERT_FALSE(q.is_empty());
	ASSERT_TRUE(q.is_full());
	EXPECT_EQ(drain(q), (std::vector<int>{10, 20, 30, 40}));
}

TEST(SpscQueueTryEmplaceRange, EmptyRangeSucceedsAndIsANoOp) {
	spsc_queue<int, 4> q;
	const std::array<int, 0> empty{};
	ASSERT_TRUE(q.try_emplace_range(empty));
	EXPECT_FALSE(q.try_dequeue().has_value());
}

TEST(SpscQueueTryEmplaceRange, InterleavesWithSingleEmplace) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace(7));
	const std::array<int, 2> src{8, 9};
	ASSERT_TRUE(q.try_emplace_range(src));

	EXPECT_EQ(drain(q), (std::vector<int>{7, 8, 9}));
}

TEST(SpscQueueTryEmplaceRange, AcceptsAVectorRange) {
	spsc_queue<int, 8> q;
	const std::vector<int> src{1, 2, 3};
	ASSERT_TRUE(q.try_emplace_range(src));
	EXPECT_EQ(drain(q), (std::vector<int>{1, 2, 3}));
}

TEST(SpscQueueTryEmplaceRange, RangeStraddlingWrapBoundaryIsReassembled) {
	spsc_queue<int, 4> q;

	// Advance the write cursor near the end of the backing array, then drain
	// so a subsequent range must wrap around the physical buffer end.
	const std::array<int, 3> warmup{1, 2, 3};
	ASSERT_TRUE(q.try_emplace_range(warmup));
	EXPECT_EQ(drain(q), (std::vector<int>{1, 2, 3}));

	const std::array<int, 4> src{4, 5, 6, 7};
	ASSERT_TRUE(q.try_emplace_range(src));
	EXPECT_EQ(drain(q), (std::vector<int>{4, 5, 6, 7}));
}

TEST(SpscQueueTryEmplaceRange, RepeatedWrapKeepsFifoOrder) {
	spsc_queue<int, 4> q;
	int next = 0;
	for (int iter = 0; iter < 100; ++iter) {
		const std::array<int, 3> src{next, next + 1, next + 2};
		ASSERT_TRUE(q.try_emplace_range(src)) << "iteration " << iter;
		EXPECT_EQ(drain(q), (std::vector<int>{next, next + 1, next + 2}))
			<< "iteration " << iter;
		next += 3;
	}
}

TEST(SpscQueueTryEmplaceRange, RejectsRangeLargerThanCapacity) {
	spsc_queue<int, 4> q;
	const std::array<int, 5> src{1, 2, 3, 4, 5}; // one past capacity N

	EXPECT_FALSE(q.try_emplace_range(src));
	EXPECT_TRUE(q.is_empty());
	EXPECT_FALSE(q.try_dequeue().has_value()); // untouched on failure
}

TEST(SpscQueueTryEmplaceRange, RejectsWhenPartiallyFull) {
	spsc_queue<int, 4> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2)); // 2 free slots remain

	const std::array<int, 3> src{3, 4, 5};
	EXPECT_FALSE(q.try_emplace_range(src));
	// Existing contents preserved, nothing from the rejected range leaks in.
	EXPECT_EQ(drain(q), (std::vector<int>{1, 2}));
}

TEST(SpscQueueTryEmplaceRange, SucceedsAgainAfterDrainingFreesSpace) {
	spsc_queue<int, 4> q;
	const std::array<int, 4> full{1, 2, 3, 4};
	ASSERT_TRUE(q.try_emplace_range(full));
	EXPECT_TRUE(q.is_full());

	const std::array<int, 2> more{5, 6};
	EXPECT_FALSE(q.try_emplace_range(more));
	EXPECT_TRUE(q.is_full());


	ASSERT_EQ(q.try_dequeue().value_or(-1), 1);
	ASSERT_EQ(q.try_dequeue().value_or(-1), 2);
	EXPECT_TRUE(q.try_emplace_range(more));
	EXPECT_TRUE(q.is_full());
	EXPECT_EQ(drain(q), (std::vector<int>{3, 4, 5, 6}));
}

} // namespace
