#include "matching_priority.fixture.hpp"
#include "order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;

namespace {

/// @brief A pro-rata book, since almost every case here needs one.
order_book pro_rata_book() {
	return order_book{1U << 10, allocation_policy::PRO_RATA};
}

} // namespace

// --------------------------------------------------------------------------
// The policy itself
// --------------------------------------------------------------------------

TEST(OrderBookAllocation, DefaultsToPriceTime) {
	const order_book book;
	EXPECT_EQ(book.policy(), allocation_policy::PRICE_TIME);
}

// --------------------------------------------------------------------------
// What both policies agree on: a sweep that takes the whole level
// --------------------------------------------------------------------------

TEST(OrderBookAllocation, FullSweepFillsEveryOrderUnderEitherPolicy) {
	for (const allocation_policy policy :
		 {allocation_policy::PRICE_TIME, allocation_policy::PRO_RATA}) {
		order_book book{1U << 10, policy};
		priority_rest_queue(book, side_t::ask, 100, {{1, 6}, {2, 3}, {3, 1}});

		const std::vector<trade> trades = book.place_order(
			{.id = 9, .side = side_t::bid, .price = 100, .qty = 10});

		EXPECT_EQ(priority_traded_for(trades, 1), 6) << to_string(policy);
		EXPECT_EQ(priority_traded_for(trades, 2), 3) << to_string(policy);
		EXPECT_EQ(priority_traded_for(trades, 3), 1) << to_string(policy);
		EXPECT_EQ(book.volume_at_price(100, side_t::ask), 0)
			<< to_string(policy);
	}
}

// --------------------------------------------------------------------------
// Pro-rata: a partial sweep divides by resting size
// --------------------------------------------------------------------------

TEST(OrderBookAllocation, PartialSweepSplitsInProportionToRestingSize) {
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{1, 60}, {2, 30}, {3, 10}});

	// Half the level, so every order gives up half of what it has resting and
	// the shares divide exactly - no residual to settle.
	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 50});

	EXPECT_EQ(priority_traded_for(trades, 1), 30);
	EXPECT_EQ(priority_traded_for(trades, 2), 15);
	EXPECT_EQ(priority_traded_for(trades, 3), 5);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 50);
}

TEST(OrderBookAllocation, TheBackOfTheQueueTradesWhileTheFrontIsUnfilled) {
	// The whole point of pro-rata, stated as the one thing price-time forbids:
	// order 3 trades even though orders 1 and 2 still have quantity resting.
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{1, 20}, {2, 20}, {3, 20}});

	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 30});

	EXPECT_EQ(priority_traded_for(trades, 3), 10);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 30);
}

TEST(OrderBookAllocation, PriceTimeGivesTheSameSweepToTheFrontOfTheQueue) {
	// The contrast case for the one above, on an identical book. Under
	// price-time the whole 30 lots stops at the head and the order at the back
	// is untouched.
	order_book book;
	priority_rest_queue(book, side_t::ask, 100, {{1, 20}, {2, 20}, {3, 20}});

	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 30});

	EXPECT_EQ(priority_traded_for(trades, 1), 20);
	EXPECT_EQ(priority_traded_for(trades, 2), 10);
	EXPECT_EQ(priority_traded_for(trades, 3), 0);
}

// --------------------------------------------------------------------------
// The rounding residual, which is where time priority still lives
// --------------------------------------------------------------------------

TEST(OrderBookAllocation, RoundingResidualGoesToTheOldestOrders) {
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{1, 10}, {2, 10}, {3, 10}});

	// A third of a 30-lot level split three ways is 3.33 lots each: flooring
	// gives 3, 3, 3 and leaves one lot over, which goes to the oldest order.
	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 10});

	EXPECT_EQ(priority_traded_for(trades, 1), 4);
	EXPECT_EQ(priority_traded_for(trades, 2), 3);
	EXPECT_EQ(priority_traded_for(trades, 3), 3);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 20);
}

TEST(OrderBookAllocation, AllocationSumsToExactlyWhatTheAggressorBrought) {
	// Deliberately awkward arithmetic: 7 lots across three 5-lot orders is 2.33
	// each. If the shares rounded to nearest, or the residual were dropped, the
	// level would print the wrong volume.
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{1, 5}, {2, 5}, {3, 5}});

	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 7});

	volume_t printed = 0;
	for (const trade &print : trades) printed += print.volume;
	EXPECT_EQ(printed, 7);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 8);
	EXPECT_EQ(priority_traded_for(trades, 1), 3); // 2 + the residual lot
	EXPECT_EQ(priority_traded_for(trades, 2), 2);
	EXPECT_EQ(priority_traded_for(trades, 3), 2);
}

TEST(OrderBookAllocation, AShareTooSmallToRoundUpToALotTradesNothing) {
	order_book book = pro_rata_book();
	// 100 lots of a 1001-lot level is 99.9 lots to the big order and 0.0999 to
	// the small one, which floors to nothing. The residual lot follows time
	// priority, and here the big order is the one that arrived first.
	priority_rest_queue(book, side_t::ask, 100, {{1, 1000}, {2, 1}});

	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 100});

	EXPECT_EQ(priority_traded_for(trades, 1), 100);
	EXPECT_EQ(priority_traded_for(trades, 2), 0);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 901);
}

TEST(OrderBookAllocation, TheResidualLotCanFillASmallOrderThatIsFirstInLine) {
	order_book book = pro_rata_book();
	// The same level with the arrival order swapped, which is the whole
	// difference: the 1-lot order now takes the residual and fills outright.
	priority_rest_queue(book, side_t::ask, 100, {{2, 1}, {1, 1000}});

	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 100});

	EXPECT_EQ(priority_traded_for(trades, 2), 1);
	EXPECT_EQ(priority_traded_for(trades, 1), 99);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 901);
}

// --------------------------------------------------------------------------
// Bookkeeping around an order the division finishes off
// --------------------------------------------------------------------------

TEST(OrderBookAllocation, AnOrderFilledMidQueueIsGoneAndCannotBeCancelled) {
	// The one thing price-time matching never does: retire an order that is not
	// the head of its level. If the index entry outlived the node, this cancel
	// would splice a cell the pool has already handed back.
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{2, 1}, {1, 1000}});

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.place_order({.id = 9, .side = side_t::bid, .price = 100, .qty = 100},
					 trades,
					 outcomes);
	ASSERT_EQ(priority_traded_for(trades, 2), 1);

	outcomes.clear();
	book.cancel_order(2, outcomes);
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes.front().type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(outcomes.front().reason, reject_reason::UNKNOWN_ORDER);

	// And the order it was queued in front of is still there, still
	// cancellable.
	outcomes.clear();
	book.cancel_order(1, outcomes);
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes.front().type, OutcomeType::CANCELLED);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 0);
}

TEST(OrderBookAllocation, EachSideOfEveryAllocationGetsItsOwnFillRecord) {
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{1, 60}, {2, 40}});

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.place_order({.id = 9, .side = side_t::bid, .price = 100, .qty = 50},
					 trades,
					 outcomes);

	// ACCEPTED, then two fills per execution - one for the resting order and
	// one for the aggressor, whose record carries the cumulative total.
	ASSERT_EQ(trades.size(), 2U);
	ASSERT_EQ(outcomes.size(), 5U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::ACCEPTED);
	EXPECT_EQ(outcomes[1].id, 1U);
	EXPECT_EQ(outcomes[2].id, 9U);
	EXPECT_EQ(outcomes[2].traded, 30);
	EXPECT_EQ(outcomes[3].id, 2U);
	EXPECT_EQ(outcomes[4].id, 9U);
	EXPECT_EQ(outcomes[4].traded, 50); // cumulative across both allocations
	EXPECT_EQ(outcomes[4].status, OrderStatus::FILLED);
}

// --------------------------------------------------------------------------
// Interaction with the rest of the book
// --------------------------------------------------------------------------

TEST(OrderBookAllocation, PriceStillBeatsSizeAcrossLevels) {
	// Price priority is not part of the policy: the better level is consumed in
	// full and only what is left over is divided at the next one.
	order_book book = pro_rata_book();
	priority_rest(book, 1, side_t::ask, 100, 10);
	priority_rest_queue(book, side_t::ask, 101, {{2, 60}, {3, 40}});

	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 101, .qty = 100});

	EXPECT_EQ(priority_traded_for(trades, 1), 10); // the whole better level
	EXPECT_EQ(priority_traded_for(trades, 2), 54); // 90 * 60/100
	EXPECT_EQ(priority_traded_for(trades, 3), 36); // 90 * 40/100
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 0);
	EXPECT_EQ(book.volume_at_price(101, side_t::ask), 10);
}

TEST(OrderBookAllocation, AnonymousDepthTakesItsShareLikeAnyOtherOrder) {
	order_book book = pro_rata_book();
	book.add_order(side_t::ask, 100, 30); // nobody's liquidity, still resting
	priority_rest(book, 1, side_t::ask, 100, 10);

	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 20});

	EXPECT_EQ(priority_traded_for(trades, 0), 15); // 20 * 30/40
	EXPECT_EQ(priority_traded_for(trades, 1), 5);  // 20 * 10/40
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 20);
}

TEST(OrderBookAllocation, AnUnfilledRemainderStillRests) {
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{1, 10}, {2, 10}});

	// Sweeps the level in full - so by the FIFO path - and rests the other 10.
	const std::vector<trade> trades = book.place_order(
		{.id = 9, .side = side_t::bid, .price = 100, .qty = 30});

	EXPECT_EQ(trades.size(), 2U);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 0);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 10);
}

TEST(OrderBookAllocation, FillOrKillStillMeasuresTheWholeCrossingDepth) {
	// The all-or-nothing pre-check adds up level aggregates and knows nothing
	// about how they would be divided - correctly, since a sweep that clears a
	// level fills every order on it either way.
	order_book book = pro_rata_book();
	priority_rest_queue(book, side_t::ask, 100, {{1, 6}, {2, 3}});

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.place_order({.id    = 9,
					  .side  = side_t::bid,
					  .tif   = time_in_force_instruction::FILL_OR_KILL,
					  .price = 100,
					  .qty   = 10},
					 trades,
					 outcomes);

	EXPECT_TRUE(trades.empty());
	ASSERT_FALSE(outcomes.empty());
	EXPECT_EQ(outcomes.front().type, OutcomeType::REJECTED);
	EXPECT_EQ(outcomes.front().reason, reject_reason::INSUFFICIENT_LIQUIDITY);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 9);
}
