#include "spsc_queue.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <thread>
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
	// Effective capacity is N (one slot reserved to distinguish full/empty).
	spsc_queue<int, 4> q;
	const std::array<int, 4> src{10, 20, 30, 40};

	ASSERT_TRUE(q.try_emplace_range(src));
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
	const std::array<int, 5> src{1, 2, 3, 4, 5}; // one past effective capacity

	EXPECT_FALSE(q.try_emplace_range(src));
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
	EXPECT_FALSE(q.try_emplace_range(more)); // full

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

TEST(SpscQueueTryPopOutParam, ReturnsFalseAndLeavesOutUntouchedWhenEmpty) {
	spsc_queue<int, 4> q;
	int v = 42;
	EXPECT_FALSE(q.try_dequeue(v));
	EXPECT_EQ(v, 42);
}

TEST(SpscQueueTryPopOutParam, KeepsFifoOrderAcrossWrapBoundary) {
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
	EXPECT_EQ(q.size(), 0u);
}

TEST(SpscQueueObservers, SizeTracksEmplaceAndPop) {
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
	EXPECT_EQ(q.size(), 0u);
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
	EXPECT_EQ(q.size(), 0u);
}

TEST(SpscQueueConcurrency, TransferSimpleValues) {
	spsc_queue<unsigned, 32> q;
	constexpr unsigned N          = 100'000;
	std::atomic_uint64_t prod_sum = 0;
	std::atomic_uint64_t cons_sum = 0;

	// FIFO integrity: the producer enqueues 0..N-1 in order, so the consumer
	// must pop them in exactly that order. Record the first deviation and
	// assert on it after the join (gtest EXPECT_* is unsafe off the main
	// thread; join() synchronises these reads).
	bool in_order            = true;
	unsigned first_bad_index = 0;
	unsigned first_bad_value = 0;
	std::thread producer{[&] {
		for (unsigned i = 0; i < N; i++) {
			while (!q.try_emplace(i)) {}
			prod_sum += i;
		}
	}};
	std::thread consumer{[&] {
		for (unsigned i = 0; i < N; i++) {
			std::optional<unsigned> v;
			do { v = q.try_dequeue(); } while (!v);
			cons_sum += *v;
			if (in_order && *v != i) {
				in_order        = false;
				first_bad_index = i;
				first_bad_value = *v;
			}
		}
	}};
	producer.join();
	consumer.join();
	EXPECT_EQ(prod_sum, cons_sum);
	EXPECT_TRUE(in_order) << "FIFO order violated at index " << first_bad_index
						  << ": expected " << first_bad_index << ", got "
						  << first_bad_value;
}

template <class T, size_t N>
std::vector<T> pop_range_all(spsc_queue<T, N> &q) {
	std::vector<T> out;
	std::array<T, N> buffer;

	while (true) {
		auto popped = q.try_dequeue_range(buffer);
		if (popped == 0) break;

		out.insert(out.end(), buffer.begin(), buffer.begin() + popped);
	}

	return out;
}

TEST(SpscQueueTryPopRange, PopsWholeBatch) {
	spsc_queue<int, 8> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2, 3, 4}));

	std::array<int, 4> out{};

	EXPECT_EQ(q.try_dequeue_range(out), 4u);

	EXPECT_EQ(out, (std::array{1, 2, 3, 4}));

	EXPECT_TRUE(q.is_empty());
}

TEST(SpscQueueTryPopRange, PopsOnlyAvailableElements) {
	spsc_queue<int, 8> q;

	ASSERT_TRUE(q.try_emplace_range(std::array{1, 2}));

	std::array<int, 4> out{};

	EXPECT_EQ(q.try_dequeue_range(out), 2u);

	EXPECT_EQ(out[0], 1);
	EXPECT_EQ(out[1], 2);

	EXPECT_TRUE(q.is_empty());
}

TEST(SpscQueueTryPopRange, EmptyQueueReturnsZero) {
	spsc_queue<int, 8> q;

	std::array<int, 8> out{};

	EXPECT_EQ(q.try_dequeue_range(out), 0u);
}

TEST(SpscQueueTryPopRange, HandlesWrapAround) {
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
