#include "strategy/backtest/scheduler.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

// The discrete-event queue. Two claims are load-bearing and the rest is
// plumbing: everything due comes out, and items due at the same instant come
// out in the order they went in. The second is the one a run's reproducibility
// rests on, so most of what follows is about it.

using namespace exchange::strategy::backtest;

namespace {

/// @brief A payload that is only an identity, which is all the ordering cases
///        need. Trivially copyable, as @c delay_queue requires.
struct scheduler_item {
	int tag;
};

using item_queue = delay_queue<scheduler_item>;

/// @brief The tags released by @p queue at @p now_ns, in release order.
std::vector<int> released_tags(item_queue &queue, std::uint64_t now_ns) {
	std::vector<scheduler_item> out;
	(void)queue.release(now_ns, out);
	std::vector<int> tags;
	tags.reserve(out.size());
	for (const scheduler_item &item : out) tags.push_back(item.tag);
	return tags;
}

} // namespace

TEST(BacktestScheduler, StartsEmpty) {
	item_queue queue{8};
	EXPECT_TRUE(queue.is_empty());
	EXPECT_EQ(queue.pending(), 0U);
	EXPECT_EQ(queue.capacity(), 8U);
	EXPECT_FALSE(queue.next_due_ns().has_value());
}

TEST(BacktestScheduler, HoldsAnItemUntilItComesDue) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(100, {.tag = 1}));

	EXPECT_EQ(queue.pending(), 1U);
	EXPECT_TRUE(released_tags(queue, 99).empty());
	EXPECT_EQ(queue.pending(), 1U) << "a release that found nothing due must "
									  "not consume anything";
	EXPECT_EQ(released_tags(queue, 100), std::vector<int>{1})
		<< "due is inclusive: an item due exactly now has arrived";
	EXPECT_TRUE(queue.is_empty());
}

TEST(BacktestScheduler, ReleasesEverythingDueInOneCall) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(20, {.tag = 2}));
	ASSERT_TRUE(queue.schedule(30, {.tag = 3}));

	EXPECT_EQ(released_tags(queue, 25), (std::vector<int>{1, 2}));
	EXPECT_EQ(queue.pending(), 1U);
}

// The ordering the class exists to provide. Scheduled latest-first so a heap
// that merely happened to preserve insertion order could not pass by accident.
TEST(BacktestScheduler, ReleasesInDueTimeOrderWhateverOrderTheyWereScheduled) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(300, {.tag = 3}));
	ASSERT_TRUE(queue.schedule(100, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(200, {.tag = 2}));

	EXPECT_EQ(released_tags(queue, 1000), (std::vector<int>{1, 2, 3}));
}

// The tiebreak. Without the sequence number these three are equal under the
// comparison and the heap may return them in any order at all - which would
// make the run's fills depend on the standard library it was built against.
TEST(BacktestScheduler, BreaksTiesInSchedulingOrder) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(100, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(100, {.tag = 2}));
	ASSERT_TRUE(queue.schedule(100, {.tag = 3}));

	EXPECT_EQ(released_tags(queue, 100), (std::vector<int>{1, 2, 3}));
}

// Enough simultaneous items to sit several levels deep in the heap, where an
// ordering that held only by luck for three would not hold. A sift that ignored
// the sequence number reorders these.
TEST(BacktestScheduler, BreaksTiesInSchedulingOrderAcrossADeepHeap) {
	item_queue queue{64};
	std::vector<int> expected;
	for (int tag = 0; tag < 40; ++tag) {
		ASSERT_TRUE(queue.schedule(500, {.tag = tag}));
		expected.push_back(tag);
	}

	EXPECT_EQ(released_tags(queue, 500), expected);
}

// The tie is broken by scheduling order and not by payload order, so two items
// whose tags descend still come out as they went in.
TEST(BacktestScheduler, DoesNotSortTiesByPayload) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(100, {.tag = 9}));
	ASSERT_TRUE(queue.schedule(100, {.tag = 4}));

	EXPECT_EQ(released_tags(queue, 100), (std::vector<int>{9, 4}));
}

// Interleaved times and ties together: the full order is (due, sequence).
TEST(BacktestScheduler, OrdersByDueTimeThenBySequence) {
	item_queue queue{16};
	ASSERT_TRUE(queue.schedule(200, {.tag = 3}));
	ASSERT_TRUE(queue.schedule(100, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(200, {.tag = 4}));
	ASSERT_TRUE(queue.schedule(100, {.tag = 2}));

	EXPECT_EQ(released_tags(queue, 1000), (std::vector<int>{1, 2, 3, 4}));
}

// One message carrying several commands arrives as one message: same due time,
// contents in the order written.
TEST(BacktestScheduler, KeepsABatchTogetherAndInOrder) {
	item_queue queue{16};
	const scheduler_item batch[] = {{.tag = 7}, {.tag = 8}, {.tag = 9}};
	ASSERT_TRUE(
		queue.schedule_range(100, std::span<const scheduler_item>{batch, 3}));

	EXPECT_EQ(queue.pending(), 3U);
	EXPECT_EQ(released_tags(queue, 100), (std::vector<int>{7, 8, 9}));
}

// A batch scheduled earlier releases ahead of one scheduled sooner but due
// later - the case a FIFO gets wrong, and the reason this is a heap.
TEST(BacktestScheduler, LetsALaterScheduleComeDueFirst) {
	item_queue queue{16};
	const scheduler_item slow[] = {{.tag = 1}, {.tag = 2}};
	const scheduler_item fast[] = {{.tag = 3}, {.tag = 4}};
	ASSERT_TRUE(
		queue.schedule_range(900, std::span<const scheduler_item>{slow, 2}));
	ASSERT_TRUE(
		queue.schedule_range(100, std::span<const scheduler_item>{fast, 2}));

	EXPECT_EQ(released_tags(queue, 1000), (std::vector<int>{3, 4, 1, 2}));
}

TEST(BacktestScheduler, RefusesAnItemBeyondCapacity) {
	item_queue queue{2};
	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(10, {.tag = 2}));

	EXPECT_FALSE(queue.schedule(10, {.tag = 3}));
	EXPECT_EQ(queue.pending(), 2U);
}

// All-or-nothing, which is what lets a caller roll back rather than work out
// which prefix of its batch got through.
TEST(BacktestScheduler, RefusesAPartiallyFittingBatchWhole) {
	item_queue queue{4};
	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	const scheduler_item batch[] = {{.tag = 2},
									{.tag = 3},
									{.tag = 4},
									{.tag = 5}};

	EXPECT_FALSE(
		queue.schedule_range(20, std::span<const scheduler_item>{batch, 4}));
	EXPECT_EQ(queue.pending(), 1U) << "none of the batch may be kept";
	EXPECT_EQ(released_tags(queue, 100), std::vector<int>{1});
}

TEST(BacktestScheduler, AcceptsAnEmptyBatchWithoutSchedulingAnything) {
	item_queue queue{4};
	EXPECT_TRUE(queue.schedule_range(10, std::span<const scheduler_item>{}));
	EXPECT_EQ(queue.pending(), 0U);
	EXPECT_EQ(queue.scheduled(), 0U);
}

// Room is freed by releasing, so a queue that filled up is usable again.
TEST(BacktestScheduler, TakesMoreOnceReleasedItemsHaveFreedRoom) {
	item_queue queue{2};
	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(10, {.tag = 2}));
	ASSERT_EQ(released_tags(queue, 10).size(), 2U);

	EXPECT_TRUE(queue.schedule(20, {.tag = 3}));
}

TEST(BacktestScheduler, AppendsToTheOutputRatherThanClearingIt) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(10, {.tag = 2}));

	std::vector<scheduler_item> out{{.tag = 1}};
	EXPECT_EQ(queue.release(10, out), 1U) << "the count is what was appended";
	ASSERT_EQ(out.size(), 2U);
	EXPECT_EQ(out[0].tag, 1)
		<< "a staged item stays in front of a new release, "
		   "which is how a stalled caller keeps order";
	EXPECT_EQ(out[1].tag, 2);
}

TEST(BacktestScheduler, ReportsWhenTheNextItemComesDue) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(300, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(100, {.tag = 2}));

	ASSERT_TRUE(queue.next_due_ns().has_value());
	EXPECT_EQ(*queue.next_due_ns(), 100U) << "the soonest, not the first added";
}

TEST(BacktestScheduler, CountsEverythingEverScheduled) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	ASSERT_TRUE(queue.schedule(20, {.tag = 2}));
	(void)released_tags(queue, 10);

	EXPECT_EQ(queue.scheduled(), 2U) << "a total, not a census of what is left";
	EXPECT_EQ(queue.pending(), 1U);
}

TEST(BacktestScheduler, ClearAbandonsWhatIsInFlight) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	queue.clear();

	EXPECT_TRUE(queue.is_empty());
	EXPECT_TRUE(released_tags(queue, 1000).empty());
}

// The sequence counter is deliberately not reset by clear: the order has to be
// total across the whole run, and a restarted counter would let an item
// scheduled after the clear tie with - and therefore be ordered against - one
// scheduled before it. Observable through `scheduled()`, which is that counter.
TEST(BacktestScheduler, KeepsTheSequenceCounterAcrossAClear) {
	item_queue queue{8};
	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	queue.clear();
	ASSERT_TRUE(queue.schedule(10, {.tag = 2}));

	EXPECT_EQ(queue.scheduled(), 2U);
}

// Exposed so a caller can decline before doing work a refusal would waste.
// @see wire::submit_range, which draws its jitter only once this says yes.
TEST(BacktestScheduler, ReportsWhetherABatchWouldFit) {
	item_queue queue{3};
	EXPECT_TRUE(queue.has_room(3));
	EXPECT_FALSE(queue.has_room(4));

	ASSERT_TRUE(queue.schedule(10, {.tag = 1}));
	EXPECT_TRUE(queue.has_room(2));
	EXPECT_FALSE(queue.has_room(3));

	EXPECT_TRUE(queue.has_room(0)) << "asking for nothing always fits";
}
