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
	const sweep_estimate sweep =
		book.estimate_sweep(side_t::ask, 100 * units::lot);

	EXPECT_FALSE(sweep.has_liquidity());
	EXPECT_FALSE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 0 * units::lot);
	EXPECT_EQ(sweep.notional, 0 * (units::tick * units::lot));
	EXPECT_EQ(sweep.levels, 0U);
}

TEST(OrderBookSweepEstimate, TakingNothingIsCompleteAndCostsNothing) {
	order_book book;
	priority_rest(book, 1, side_t::ask, at_tick(100), 10 * units::lot);

	const sweep_estimate zero =
		book.estimate_sweep(side_t::ask, 0 * units::lot);
	EXPECT_TRUE(zero.is_complete());
	EXPECT_FALSE(zero.has_liquidity());
	EXPECT_EQ(zero.notional, 0 * (units::tick * units::lot));
	EXPECT_EQ(zero.slippage(), 0 * (units::tick * units::lot));
}

// --------------------------------------------------------------------------
// Walking the ladder, and what it costs
// --------------------------------------------------------------------------

TEST(OrderBookSweepEstimate, SizeInsideTheTouchPaysTheTouchPrice) {
	order_book book;
	priority_rest(book, 1, side_t::ask, at_tick(100), 10 * units::lot);

	const sweep_estimate sweep =
		book.estimate_sweep(side_t::ask, 4 * units::lot);
	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.levels, 1U);
	EXPECT_EQ(sweep.touch, at_tick(100));
	EXPECT_EQ(sweep.last, at_tick(100));
	EXPECT_EQ(sweep.notional, 400 * (units::tick * units::lot))
		<< "4 lots at 100 ticks";
	EXPECT_EQ(sweep.impact(), 0 * units::tick);
	EXPECT_EQ(sweep.slippage(), 0 * (units::tick * units::lot))
		<< "nothing paid above the touch";
}

TEST(OrderBookSweepEstimate, CostIsSummedPerLevelNotTakenAtTheWorstPrice) {
	// The distinction the notional exists for: 25 lots across 100/101/102 costs
	// 10*100 + 10*101 + 5*102, which is less than 25 lots at the price the
	// sweep ends on. A model that charged the last price would overstate this
	// by 35 tick-lots.
	order_book book;
	priority_rest(book, 1, side_t::ask, at_tick(100), 10 * units::lot);
	priority_rest(book, 2, side_t::ask, at_tick(101), 10 * units::lot);
	priority_rest(book, 3, side_t::ask, at_tick(102), 10 * units::lot);

	const sweep_estimate sweep =
		book.estimate_sweep(side_t::ask, 25 * units::lot);
	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 25 * units::lot);
	EXPECT_EQ(sweep.levels, 3U);
	EXPECT_EQ(sweep.last, at_tick(102));
	EXPECT_EQ(sweep.notional,
			  notional_t{(1000 + 1010 + 510) * (units::tick * units::lot)});
	EXPECT_EQ(sweep.impact(), 2 * units::tick);
	EXPECT_EQ(sweep.slippage(), 20 * (units::tick * units::lot))
		<< "2520 paid, 2500 at the touch";
}

TEST(OrderBookSweepEstimate, ASizeEndingOnALevelBoundaryDoesNotTouchTheNext) {
	order_book book;
	priority_rest(book, 1, side_t::ask, at_tick(100), 10 * units::lot);
	priority_rest(book, 2, side_t::ask, at_tick(101), 10 * units::lot);
	priority_rest(book, 3, side_t::ask, at_tick(102), 10 * units::lot);

	const sweep_estimate sweep =
		book.estimate_sweep(side_t::ask, 20 * units::lot);
	EXPECT_EQ(sweep.levels, 2U);
	EXPECT_EQ(sweep.last, at_tick(101));
	EXPECT_EQ(sweep.notional,
			  notional_t{(1000 + 1010) * (units::tick * units::lot)});
}

TEST(OrderBookSweepEstimate, RunsOutOfDepthRatherThanInventingIt) {
	order_book book;
	priority_rest(book, 1, side_t::ask, at_tick(100), 10 * units::lot);

	const sweep_estimate sweep =
		book.estimate_sweep(side_t::ask, 40 * units::lot);
	EXPECT_FALSE(sweep.is_complete());
	EXPECT_EQ(sweep.requested, 40 * units::lot);
	EXPECT_EQ(sweep.filled, 10 * units::lot);
	EXPECT_EQ(sweep.notional, 1000 * (units::tick * units::lot))
		<< "only the depth that was there";
}

TEST(OrderBookSweepEstimate, ManyOrdersAtOnePriceAreOneLevel) {
	// Level count is a statement about prices, not about participants: this is
	// the number that says how far a sweep reaches.
	order_book book;
	priority_rest_queue(
		book,
		side_t::ask,
		at_tick(100),
		std::to_array<priority_quote>(
			{{1, 5 * units::lot}, {2, 5 * units::lot}, {3, 5 * units::lot}}));

	const sweep_estimate sweep =
		book.estimate_sweep(side_t::ask, 15 * units::lot);
	EXPECT_EQ(sweep.levels, 1U);
	EXPECT_EQ(sweep.filled, 15 * units::lot);
	EXPECT_EQ(sweep.impact(), 0 * units::tick);
}

// --------------------------------------------------------------------------
// The seller's side
// --------------------------------------------------------------------------

TEST(OrderBookSweepEstimate, SlippageAndImpactStayPositiveSellingIntoBids) {
	order_book book;
	priority_rest(book, 1, side_t::bid, at_tick(100), 10 * units::lot);
	priority_rest(book, 2, side_t::bid, at_tick(99), 10 * units::lot);

	const sweep_estimate sweep =
		book.estimate_sweep(side_t::bid, 15 * units::lot);
	EXPECT_EQ(sweep.side, side_t::bid);
	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.touch, at_tick(100));
	EXPECT_EQ(sweep.last, at_tick(99)) << "a seller walks down the bids";
	EXPECT_EQ(sweep.impact(), 1 * units::tick);
	EXPECT_EQ(sweep.notional,
			  notional_t{(1000 + 495) * (units::tick * units::lot)});
	EXPECT_EQ(sweep.slippage(), 5 * (units::tick * units::lot))
		<< "1500 at the touch, 1495 received";
}

TEST(OrderBookSweepEstimate, ImpactAndSlippageDisagreeAndBothAreRight) {
	// One lot deep at the touch and a wall ten ticks away. Impact is the whole
	// ten, because the last lot really does reach that far; slippage is small
	// relative to the size, because almost nothing filled at the touch. A
	// caller watching only one of the two draws the wrong conclusion here.
	order_book book;
	priority_rest(book, 1, side_t::ask, at_tick(100), 1 * units::lot);
	priority_rest(book, 2, side_t::ask, at_tick(110), 100 * units::lot);

	const sweep_estimate sweep =
		book.estimate_sweep(side_t::ask, 11 * units::lot);
	EXPECT_EQ(sweep.impact(), 10 * units::tick);
	EXPECT_EQ(sweep.notional,
			  notional_t{(100 + 1100) * (units::tick * units::lot)});
	EXPECT_EQ(sweep.slippage(), 100 * (units::tick * units::lot))
		<< "1200 paid against 1100 at the touch";
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
							at_tick(100),
							std::to_array<priority_quote>(
								{{1, 6 * units::lot}, {2, 4 * units::lot}}));
		priority_rest_queue(book,
							side_t::ask,
							at_tick(101),
							std::to_array<priority_quote>(
								{{3, 10 * units::lot}, {4, 10 * units::lot}}));

		constexpr volume_t SIZE     = 15 * units::lot;
		const sweep_estimate before = book.estimate_sweep(side_t::ask, SIZE);

		const std::vector<trade> trades =
			book.place_order({.id    = 9,
							  .side  = side_t::bid,
							  .price = at_tick(101),
							  .qty   = order_quantity(SIZE)});

		volume_t filled = {};
		notional_t paid = {};
		for (const trade &print : trades) {
			filled += print.volume;
			paid += notional_of(print.price, print.volume);
		}

		EXPECT_EQ(before.filled, filled) << to_string(policy);
		EXPECT_EQ(before.notional, paid) << to_string(policy);
		EXPECT_EQ(before.last, at_tick(101)) << to_string(policy);
	}
}
