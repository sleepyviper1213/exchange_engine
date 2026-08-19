#include "core/concurrency/lockfree/spsc_queue.hpp"
#include "core/util/counted.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

using exchange::core::concurrency::lockfree::spsc_queue;
using exchange::core::util::counted;

// --------------------------------------------------------------------------
// Lifetime: construction/destruction must balance
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, PushPopPreservesValueAndBalancesLifetime) {
	const int base = counted::alive;
	{
		spsc_queue<counted, 4> q;
		ASSERT_TRUE(q.try_emplace(7));
		EXPECT_EQ(q.size(), 1u);

		auto v = q.try_dequeue();
		ASSERT_TRUE(v.has_value());
		EXPECT_EQ(v->value, 7);
		EXPECT_TRUE(q.is_empty());
	}
	EXPECT_EQ(counted::alive, base); // no leak, no double-destroy
}

TEST(SpscQueueNonPod, DestructorDestroysUnconsumedElements) {
	const int base = counted::alive;
	{
		spsc_queue<counted, 8> q;
		ASSERT_TRUE(q.try_emplace(1));
		ASSERT_TRUE(q.try_emplace(2));
		ASSERT_TRUE(q.try_emplace(3));
		EXPECT_EQ(counted::alive, base + 3);
		// Deliberately leave all three enqueued: the destructor must clean up.
	}
	EXPECT_EQ(counted::alive, base);
}

TEST(SpscQueueNonPod, ClearDestroysPendingAndQueueStaysUsable) {
	const int base = counted::alive;
	spsc_queue<counted, 4> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));
	EXPECT_EQ(counted::alive, base + 2);

	q.clear();
	EXPECT_EQ(counted::alive, base); // pending elements destroyed
	EXPECT_TRUE(q.is_empty());

	ASSERT_TRUE(q.try_emplace(9));   // reusable after clear
	const auto v = q.try_dequeue();
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(v->value, 9);
}

TEST(SpscQueueNonPod, RejectsWhenFullWithoutConstructing) {
	const int base = counted::alive;
	spsc_queue<counted, 2> q;
	ASSERT_TRUE(q.try_emplace(1));
	ASSERT_TRUE(q.try_emplace(2));       // at capacity (N == 2)
	EXPECT_EQ(counted::alive, base + 2);

	EXPECT_FALSE(q.try_emplace(3));      // rejected
	EXPECT_EQ(counted::alive, base + 2); // nothing constructed on failure
}

// --------------------------------------------------------------------------
// FIFO order with a non-trivial type, exercising the wrap boundary
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, KeepsFifoOrderAcrossWrap) {
	spsc_queue<counted, 4> q;
	int next = 0;
	for (int iter = 0; iter < 50; ++iter) {
		ASSERT_TRUE(q.try_emplace(next)) << "iteration " << iter;
		ASSERT_TRUE(q.try_emplace(next + 1)) << "iteration " << iter;

		counted a{-1};
		counted b{-1};
		ASSERT_TRUE(q.try_dequeue(a)) << "iteration " << iter;
		ASSERT_TRUE(q.try_dequeue(b)) << "iteration " << iter;
		EXPECT_EQ(a.value, next);
		EXPECT_EQ(b.value, next + 1);
		next += 2;
	}
}

// --------------------------------------------------------------------------
// Move-only element type
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, SupportsMoveOnlyTypeViaOptionalPop) {
	spsc_queue<std::unique_ptr<int>, 4> q;
	ASSERT_TRUE(q.try_emplace(std::make_unique<int>(42)));

	auto dequeued = q.try_dequeue();
	ASSERT_TRUE(dequeued.has_value());
	ASSERT_NE(*dequeued, nullptr);
	EXPECT_EQ(**dequeued, 42);
}

TEST(SpscQueueNonPod, SupportsMoveOnlyTypeViaOutParamPop) {
	spsc_queue<std::unique_ptr<int>, 4> q;
	ASSERT_TRUE(q.try_emplace(std::make_unique<int>(7)));

	std::unique_ptr<int> out;
	ASSERT_TRUE(q.try_dequeue(out));
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(*out, 7);
}

// --------------------------------------------------------------------------
// try_emplace_range with a non-trivial (copy) element type
// --------------------------------------------------------------------------

// counted, not std::string: try_emplace_range requires nothrow construction
// from the source range, and a copy that allocates cannot promise that.
TEST(SpscQueueNonPod, EmplaceRangeCopiesNonTrivialElements) {
	spsc_queue<counted, 8> q;
	const std::array<counted, 3> src{counted{1}, counted{2}, counted{3}};

	ASSERT_TRUE(q.try_emplace_range(src));
	// Copied, not moved - a move would have zapped the source values to -1.
	EXPECT_EQ(src[0].value, 1);
	EXPECT_EQ(src[1].value, 2);
	EXPECT_EQ(src[2].value, 3);

	counted out{-1};
	ASSERT_TRUE(q.try_dequeue(out));
	EXPECT_EQ(out.value, 1);
	ASSERT_TRUE(q.try_dequeue(out));
	EXPECT_EQ(out.value, 2);
	ASSERT_TRUE(q.try_dequeue(out));
	EXPECT_EQ(out.value, 3);
	EXPECT_FALSE(q.try_dequeue(out));
}

TEST(SpscQueueNonPod, EmplaceRangeConstructsAcrossWrapInOrder) {
	spsc_queue<counted, 4> q;

	// Advance the write cursor near the physical end, then drain, so the next
	// range must wrap around the buffer end in the element-wise construct path.
	const std::array<counted, 3> warmup{counted{1}, counted{2}, counted{3}};
	ASSERT_TRUE(q.try_emplace_range(warmup));
	counted sink{-1};
	for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_dequeue(sink));

	const std::array<counted, 4> src{counted{7},
									 counted{8},
									 counted{9},
									 counted{10}};
	ASSERT_TRUE(q.try_emplace_range(src));

	counted out{-1};
	for (const auto &expected : src) {
		ASSERT_TRUE(q.try_dequeue(out));
		EXPECT_EQ(out.value, expected.value);
	}
}

// --------------------------------------------------------------------------
// try_dequeue_range with a non-trivial element type
//
// A trivially copyable T is bulk-copied out by memcpy and the ring cells are
// simply abandoned; a non-trivial T takes the element-wise branch, which
// move-assigns into the caller's buffer and then destroys each cell. Only this
// branch can leak or double-destroy, so it is checked against a live count.
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, DequeueRangeMovesOutAndDestroysTheRingCells) {
	const int base = counted::alive;
	{
		spsc_queue<counted, 8> q;
		ASSERT_TRUE(q.try_emplace(1));
		ASSERT_TRUE(q.try_emplace(2));
		ASSERT_TRUE(q.try_emplace(3));

		// The destination elements already exist: the batch path assigns into
		// them rather than constructing, so only the ring cells go away.
		std::array<counted, 4> out{counted{-1},
								   counted{-1},
								   counted{-1},
								   counted{-1}};
		const int before = counted::alive;

		EXPECT_EQ(q.try_dequeue_range(out), 3u);
		EXPECT_EQ(counted::alive, before - 3);

		EXPECT_EQ(out[0].value, 1);
		EXPECT_EQ(out[1].value, 2);
		EXPECT_EQ(out[2].value, 3);
		EXPECT_EQ(out[3].value, -1); // untouched past the returned count
		EXPECT_TRUE(q.is_empty());
	}
	EXPECT_EQ(counted::alive, base);
}

TEST(SpscQueueNonPod, DequeueRangeMovesOutAcrossWrapInOrder) {
	spsc_queue<std::string, 4> q;

	// Advance the cursors near the physical end, then drain, so the next batch
	// straddles the buffer end in the element-wise dequeue path. The setup
	// pushes one at a time: std::string allocates on copy, so it cannot go
	// through try_emplace_range - which is fine, this test is about the
	// dequeue side.
	std::string sink;
	for (const char *s : {"a", "b", "c"})
		ASSERT_TRUE(q.try_emplace(std::string(s)));
	for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_dequeue(sink));

	const std::array<std::string, 4> src{"w", "x", "y", "z"};
	for (const auto &s : src) ASSERT_TRUE(q.try_emplace(std::string(s)));

	std::array<std::string, 4> out{};
	EXPECT_EQ(q.try_dequeue_range(out), 4u);
	EXPECT_EQ(out, src);
	EXPECT_TRUE(q.is_empty());
}

// --------------------------------------------------------------------------
// consume_up_to / consume_all with a non-trivial element type: each element is
// destroyed in place after the callback sees it.
// --------------------------------------------------------------------------

TEST(SpscQueueNonPod, ConsumeUpToDestroysOnlyWhatItConsumed) {
	const int base = counted::alive;
	{
		spsc_queue<counted, 8> q;
		for (int i = 1; i <= 4; ++i) ASSERT_TRUE(q.try_emplace(i));
		ASSERT_EQ(counted::alive, base + 4);

		// Fixed storage, so the callback honours its no-allocation
		// precondition.
		std::array<int, 4> seen{};
		size_t n = 0;
		EXPECT_EQ(
			q.consume_up_to(2,
							[&](counted &c) noexcept { seen[n++] = c.value; }),
			2u);

		EXPECT_EQ(seen[0], 1);
		EXPECT_EQ(seen[1], 2);
		EXPECT_EQ(counted::alive, base + 2); // the consumed two were destroyed
		EXPECT_EQ(q.size(), 2u);
	}
	EXPECT_EQ(counted::alive, base); // the destructor reclaimed the remainder
}

TEST(SpscQueueNonPod, ConsumeAllDestroysEveryElement) {
	const int base = counted::alive;
	{
		spsc_queue<counted, 8> q;
		for (int i = 1; i <= 3; ++i) ASSERT_TRUE(q.try_emplace(i));

		std::array<int, 3> seen{};
		size_t n = 0;
		EXPECT_EQ(
			q.consume_all([&](counted &c) noexcept { seen[n++] = c.value; }),
			3u);

		EXPECT_EQ(seen, (std::array{1, 2, 3}));
		EXPECT_TRUE(q.is_empty());
		EXPECT_EQ(counted::alive, base);
	}
	EXPECT_EQ(counted::alive, base);
}

TEST(SpscQueueNonPod, ConsumeUpToSpansTheWrapInOrder) {
	spsc_queue<std::string, 4> q;

	std::string sink;
	for (const char *s : {"a", "b", "c"})
		ASSERT_TRUE(q.try_emplace(std::string(s)));
	for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_dequeue(sink));

	const std::array<std::string, 4> src{"w", "x", "y", "z"};
	for (const auto &s : src) ASSERT_TRUE(q.try_emplace(std::string(s)));

	std::array<std::string, 4> seen{};
	size_t n = 0;
	EXPECT_EQ(q.consume_up_to(
				  4,
				  [&](std::string &s) noexcept { seen[n++] = std::move(s); }),
			  4u);
	EXPECT_EQ(seen, src);
	EXPECT_TRUE(q.is_empty());
}

// --------------------------------------------------------------------------
// destroy_range across the wrap boundary: clear() and the destructor re-mask
// each cursor to its physical slot, so a live range that straddles the end of
// the backing array must still be reclaimed exactly once.
// --------------------------------------------------------------------------

namespace {
/// @brief Leave @p q holding a live range that wraps the physical buffer end.
/// @details Fills, drains, then refills to capacity, so the surviving elements
/// start near the end of the backing array and continue from its front.
void fill_across_wrap(spsc_queue<counted, 4> &q) {
	{
		counted sink{-1};
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_emplace(i));
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_dequeue(sink));
	}
	for (int i = 10; i < 14; ++i) ASSERT_TRUE(q.try_emplace(i));
	ASSERT_EQ(q.size(), 4u);
}
} // namespace

TEST(SpscQueueNonPod, ClearDestroysAWrappedRange) {
	const int base = counted::alive;
	{
		spsc_queue<counted, 4> q;
		fill_across_wrap(q);
		ASSERT_EQ(counted::alive, base + 4);

		q.clear();
		EXPECT_TRUE(q.is_empty());
		EXPECT_EQ(counted::alive, base);
	}
	EXPECT_EQ(counted::alive, base);
}

TEST(SpscQueueNonPod, DestructorDestroysAWrappedRange) {
	const int base = counted::alive;
	{
		spsc_queue<counted, 4> q;
		fill_across_wrap(q);
		ASSERT_EQ(counted::alive, base + 4);
		// Left enqueued on purpose: ~spsc_queue must reach across the wrap.
	}
	EXPECT_EQ(counted::alive, base);
}

TEST(SpscQueueNonPod, PopFromEmptyLeavesOutParamUntouched) {
	spsc_queue<std::string, 4> q;
	EXPECT_FALSE(q.try_dequeue().has_value());

	std::string out = "keep";
	EXPECT_FALSE(q.try_dequeue(out));
	EXPECT_EQ(out, "keep");
}
