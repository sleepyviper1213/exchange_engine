#include "matching_priority.fixture.hpp"
#include "order_book.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;

// --------------------------------------------------------------------------
// Degenerate shapes
// --------------------------------------------------------------------------

TEST(OrderBookSweepEstimate, AnEmptySideSuppliesNothing) {
	const order_book book;
	const sweep_estimate sweep = book.estimate_sweep(side_t::ask, 100);

	EXPECT_FALSE(sweep.has_liquidity());
	EXPECT_FALSE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 0);
	EXPECT_EQ(sweep.notional, 0);
	EXPECT_EQ(sweep.levels, 0U);
}

TEST(OrderBookSweepEstimate, TakingNothingIsCompleteAndCostsNothing) {
	order_book book;
	priority_rest(book, 1, side_t::ask, 100, 10);

	const sweep_estimate zero = book.estimate_sweep(side_t::ask, 0);
	EXPECT_TRUE(zero.is_complete());
	EXPECT_FALSE(zero.has_liquidity());
	EXPECT_EQ(zero.notional, 0);
	EXPECT_EQ(zero.slippage(), 0);
}

// --------------------------------------------------------------------------
// Walking the ladder, and what it costs
// --------------------------------------------------------------------------

TEST(OrderBookSweepEstimate, SizeInsideTheTouchPaysTheTouchPrice) {
	order_book book;
	priority_rest(book, 1, side_t::ask, 100, 10);

	const sweep_estimate sweep = book.estimate_sweep(side_t::ask, 4);
	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.levels, 1U);
	EXPECT_EQ(sweep.touch, 100U);
	EXPECT_EQ(sweep.last, 100U);
	EXPECT_EQ(sweep.notional, 400) << "4 lots at 100 ticks";
	EXPECT_EQ(sweep.impact(), 0U);
	EXPECT_EQ(sweep.slippage(), 0) << "nothing paid above the touch";
}

TEST(OrderBookSweepEstimate, CostIsSummedPerLevelNotTakenAtTheWorstPrice) {
	// The distinction the notional exists for: 25 lots across 100/101/102 costs
	// 10*100 + 10*101 + 5*102, which is less than 25 lots at the price the
	// sweep ends on. A model that charged the last price would overstate this
	// by 35 tick-lots.
	order_book book;
	priority_rest(book, 1, side_t::ask, 100, 10);
	priority_rest(book, 2, side_t::ask, 101, 10);
	priority_rest(book, 3, side_t::ask, 102, 10);

	const sweep_estimate sweep = book.estimate_sweep(side_t::ask, 25);
	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 25);
	EXPECT_EQ(sweep.levels, 3U);
	EXPECT_EQ(sweep.last, 102U);
	EXPECT_EQ(sweep.notional, 1000 + 1010 + 510);
	EXPECT_EQ(sweep.impact(), 2U);
	EXPECT_EQ(sweep.slippage(), 20) << "2520 paid, 2500 at the touch";
}

TEST(OrderBookSweepEstimate, ASizeEndingOnALevelBoundaryDoesNotTouchTheNext) {
	order_book book;
	priority_rest(book, 1, side_t::ask, 100, 10);
	priority_rest(book, 2, side_t::ask, 101, 10);
	priority_rest(book, 3, side_t::ask, 102, 10);

	const sweep_estimate sweep = book.estimate_sweep(side_t::ask, 20);
	EXPECT_EQ(sweep.levels, 2U);
	EXPECT_EQ(sweep.last, 101U);
	EXPECT_EQ(sweep.notional, 1000 + 1010);
}

TEST(OrderBookSweepEstimate, RunsOutOfDepthRatherThanInventingIt) {
	order_book book;
	priority_rest(book, 1, side_t::ask, 100, 10);

	const sweep_estimate sweep = book.estimate_sweep(side_t::ask, 40);
	EXPECT_FALSE(sweep.is_complete());
	EXPECT_EQ(sweep.requested, 40);
	EXPECT_EQ(sweep.filled, 10);
	EXPECT_EQ(sweep.notional, 1000) << "only the depth that was there";
}

TEST(OrderBookSweepEstimate, ManyOrdersAtOnePriceAreOneLevel) {
	// Level count is a statement about prices, not about participants: this is
	// the number that says how far a sweep reaches.
	order_book book;
	priority_rest_queue(
		book,
		side_t::ask,
		100,
		std::to_array<priority_quote>({{1, 5}, {2, 5}, {3, 5}}));

	const sweep_estimate sweep = book.estimate_sweep(side_t::ask, 15);
	EXPECT_EQ(sweep.levels, 1U);
	EXPECT_EQ(sweep.filled, 15);
	EXPECT_EQ(sweep.impact(), 0U);
}

// --------------------------------------------------------------------------
// The seller's side
// --------------------------------------------------------------------------

TEST(OrderBookSweepEstimate, SlippageAndImpactStayPositiveSellingIntoBids) {
	order_book book;
	priority_rest(book, 1, side_t::bid, 100, 10);
	priority_rest(book, 2, side_t::bid, 99, 10);

	const sweep_estimate sweep = book.estimate_sweep(side_t::bid, 15);
	EXPECT_EQ(sweep.side, side_t::bid);
	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.touch, 100U);
	EXPECT_EQ(sweep.last, 99U) << "a seller walks down the bids";
	EXPECT_EQ(sweep.impact(), 1U);
	EXPECT_EQ(sweep.notional, 1000 + 495);
	EXPECT_EQ(sweep.slippage(), 5) << "1500 at the touch, 1495 received";
}

TEST(OrderBookSweepEstimate, ImpactAndSlippageDisagreeAndBothAreRight) {
	// One lot deep at the touch and a wall ten ticks away. Impact is the whole
	// ten, because the last lot really does reach that far; slippage is small
	// relative to the size, because almost nothing filled at the touch. A
	// caller watching only one of the two draws the wrong conclusion here.
	order_book book;
	priority_rest(book, 1, side_t::ask, 100, 1);
	priority_rest(book, 2, side_t::ask, 110, 100);

	const sweep_estimate sweep = book.estimate_sweep(side_t::ask, 11);
	EXPECT_EQ(sweep.impact(), 10U);
	EXPECT_EQ(sweep.notional, 100 + 1100);
	EXPECT_EQ(sweep.slippage(), 100) << "1200 paid against 1100 at the touch";
}

// --------------------------------------------------------------------------
// Agreement with what matching actually does
// --------------------------------------------------------------------------

TEST(OrderBookSweepEstimate, TheEstimateIsWhatTheSweepThenPays) {
	// The estimate claims to walk the ladder the way the matching loop does, so
	// the test executes the sweep and adds the prints up. Both policies: the
	// allocation rule decides who fills, never what the taker pays.
	for (const allocation_policy policy :
		 {allocation_policy::PRICE_TIME, allocation_policy::PRO_RATA}) {
		order_book book{1U << 10, policy};
		priority_rest_queue(book,
							side_t::ask,
							100,
							std::to_array<priority_quote>({{1, 6}, {2, 4}}));
		priority_rest_queue(book,
							side_t::ask,
							101,
							std::to_array<priority_quote>({{3, 10}, {4, 10}}));

		constexpr volume_t SIZE     = 15;
		const sweep_estimate before = book.estimate_sweep(side_t::ask, SIZE);

		const std::vector<trade> trades = book.place_order(
			{.id = 9, .side = side_t::bid, .price = 101, .qty = SIZE});

		volume_t filled = 0;
		volume_t paid   = 0;
		for (const trade &print : trades) {
			filled += print.volume;
			paid += static_cast<volume_t>(print.price) * print.volume;
		}

		EXPECT_EQ(before.filled, filled) << to_string(policy);
		EXPECT_EQ(before.notional, paid) << to_string(policy);
		EXPECT_EQ(before.last, 101U) << to_string(policy);
	}
}
