#include "lockfree/queue/spsc_queue.hpp"

#include <gtest/gtest.h>

using namespace core::lockfree;

#include <array>
#include <utility>
#include <vector>

namespace {
/// @brief Drain the queue into a vector, preserving FIFO order.
template <class T, size_t N>
std::vector<T> drain(spsc_queue<T, N> &q) {
	std::vector<T> out;
	while (auto v = q.try_dequeue()) out.emplace_back(*v);
	return out;
}
} // namespace

// --------------------------------------------------------------------------
// try_emplace_range — happy paths
// --------------------------------------------------------------------------

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
	ASSERT_TRUE(q.is_full()) ;
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

// --------------------------------------------------------------------------
// try_emplace_range — wrap-around
// --------------------------------------------------------------------------

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

// --------------------------------------------------------------------------
// try_emplace_range — unhappy paths (reservation is all-or-nothing)
// --------------------------------------------------------------------------

TEST(SpscQueueTryEmplaceRange, RejectsRangeLargerThanCapacity) {
	spsc_queue<int, 4> q;
	const std::array<int, 5> src{1, 2, 3, 4, 5}; // one past capacity N

	EXPECT_FALSE(q.try_emplace_range(src));
	EXPECT_TRUE(q.is_empty());
	EXPECT_FALSE(q.try_dequeue().has_value());   // untouched on failure
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

// --------------------------------------------------------------------------
// try_dequeue(T&) — out-parameter overload
// --------------------------------------------------------------------------

TEST(SpscQueueTryPopOutParam, PopsElementsInFifoOrder) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	ASSERT_TRUE(q.try_emplace(3));

	int v = 0;
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 1);
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 2);
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 3);
	EXPECT_FALSE(q.try_dequeue(v));
	EXPECT_TRUE(q.is_empty());
}

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

// --------------------------------------------------------------------------
// Observers: is_empty() / size()
// --------------------------------------------------------------------------

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

// --------------------------------------------------------------------------
// clear()
// --------------------------------------------------------------------------

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

	// The queue empties, so record the delivered values through a nothrow
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

// --------------------------------------------------------------------------
// try_emplace — single-element producer path
// --------------------------------------------------------------------------

TEST(SpscQueueTryEmplace, FillsToCapacityThenRejectsWhenFull) {
	spsc_queue<int, 4> q;
	for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.try_emplace(i)) << "slot " << i;

	EXPECT_TRUE(q.is_full());
	EXPECT_FALSE(q.try_emplace(99));
	EXPECT_EQ(q.size(), 4u);
	// The rejected value never entered the ring.
	EXPECT_EQ(drain(q), (std::vector{0, 1, 2, 3}));
}

TEST(SpscQueueTryEmplace, KeepsFifoOrder) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	ASSERT_TRUE(q.try_emplace(3));

	EXPECT_EQ(drain(q), (std::vector{1, 2, 3}));
}

TEST(SpscQueueTryEmplace, AcceptsAgainAfterDequeueFreesASlot) {
	spsc_queue<int, 2> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	ASSERT_TRUE(q.is_full());
	EXPECT_FALSE(q.try_emplace(3));

	int v = 0;
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_EQ(v, 1);
	EXPECT_TRUE(q.try_emplace(3));

	EXPECT_EQ(drain(q), (std::vector{2, 3}));
}

TEST(SpscQueueTryEmplace, ForwardsMultipleConstructorArguments) {
	spsc_queue<std::pair<int, int>, 4> q;
	ASSERT_TRUE(q.try_emplace(1, 2));

	const auto v = q.try_dequeue();
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(v->first, 1);
	EXPECT_EQ(v->second, 2);
}

// --------------------------------------------------------------------------
// try_dequeue() — optional-returning overload
// --------------------------------------------------------------------------

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

// --------------------------------------------------------------------------
// is_full()
// --------------------------------------------------------------------------

TEST(SpscQueueObservers, IsFullOnlyWhenAllSlotsTaken) {
	spsc_queue<int, 4> q;
	EXPECT_FALSE(q.is_full());

	for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.try_emplace(i));
	EXPECT_TRUE(q.is_full());

	int v = 0;
	ASSERT_TRUE(q.try_dequeue(v));
	EXPECT_FALSE(q.is_full());
}

// --------------------------------------------------------------------------
// try_dequeue_range — buffer smaller than the available count
// --------------------------------------------------------------------------

TEST(SpscQueueTryDequeueRange, CapsAtBufferSizeWhenMoreAvailable) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3, 4}));

	std::array<int, 2> out{};
	EXPECT_EQ(q.try_dequeue_range(out), 2u);
	EXPECT_EQ(out, (std::array{1, 2}));
	EXPECT_EQ(q.size(), 2u);
	EXPECT_EQ(drain(q), (std::vector{3, 4}));
}

// --------------------------------------------------------------------------
// consume_up_to — zero limit
// --------------------------------------------------------------------------

TEST(SpscQueueConsumeUpTo, ZeroLimitConsumesNothing) {
	spsc_queue<int, 8> q;
	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3}));

	EXPECT_EQ(q.consume_up_to(0, [](int &) noexcept {}), 0u);
	EXPECT_EQ(q.size(), 3u);
	EXPECT_EQ(drain(q), (std::vector{1, 2, 3}));
}
