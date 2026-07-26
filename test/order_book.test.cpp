#include "trading-engine/order_book.hpp"

#include "core/optimisation/branchless_binary_search.hpp"

#include <gtest/gtest.h>

using namespace exchange::engine;
using namespace exchange;

// --------------------------------------------------------------------------
// Queries / single-sided helpers
// --------------------------------------------------------------------------

TEST(OrderBook, VolumeAtPriceAggregatesAndReportsZeroForEmpty) {
	order_book ob;
	ob.add_order(Side::BID, 100, 10);
	ob.add_order(Side::BID, 100, 5);

	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 15);
	EXPECT_EQ(ob.volume_at_price(99, Side::BID), 0);
	EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 0);
}

TEST(OrderBook, BestBidAskAreNulloptOnEmptyBook) {
	order_book ob;
	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());

	ob.add_order(Side::BID, 100, 10);
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), 100u);
	EXPECT_FALSE(ob.best_ask().has_value());
}

// --------------------------------------------------------------------------
// cancel_order (O(1) by id)
// --------------------------------------------------------------------------

TEST(OrderBook, CancelRemovesRestingOrder) {
	order_book ob;
	ob.place_order({.id     = 1,
					.side   = Side::BID,
					.price  = 100,
					.volume = 10}); // no opposite -> rests
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 10);

	ob.cancel_order(1);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 0);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, CancelUnknownIdIsNoOp) {
	order_book ob;
	ob.place_order({.id = 1, .side = Side::BID, .price = 100, .volume = 10});
	ob.cancel_order(999); // unknown
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 10);
}

TEST(OrderBook, CancelOneOfTwoAtSameLevelKeepsTheOther) {
	order_book ob;
	ob.place_order({.id = 1, .side = Side::BID, .price = 100, .volume = 10});
	ob.place_order({.id = 2, .side = Side::BID, .price = 100, .volume = 7});

	ob.cancel_order(1);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 7);
}

// --------------------------------------------------------------------------
// delete_order — reduce resting volume FIFO-first
// --------------------------------------------------------------------------

TEST(OrderBook, DeletePartialReducesVolume) {
	order_book ob;
	ob.add_order(Side::BID, 100, 10);
	ob.delete_order(Side::BID, 100, 4);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 6);
}

TEST(OrderBook, DeleteFullVolumeRemovesLevel) {
	order_book ob;
	ob.add_order(Side::BID, 100, 10);
	ob.delete_order(Side::BID, 100, 10);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 0);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, DeleteSpanningTwoOrdersDrainsFifoFirst) {
	order_book ob;
	ob.add_order(Side::BID, 100, 4);    // oldest
	ob.add_order(Side::BID, 100, 6);    // newest
	ob.delete_order(Side::BID, 100, 7); // drains first (4) + 3 of the second
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 3);
}

// --------------------------------------------------------------------------
// set_level — absolute L2 diff-feed primitive
// --------------------------------------------------------------------------

TEST(OrderBook, SetLevelCreatesLevel) {
	order_book ob;
	ob.set_level(Side::BID, 100, 25);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 25);
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), 100u);
}

TEST(OrderBook, SetLevelOverwritesAbsoluteVolume) {
	order_book ob;
	ob.set_level(Side::ASK, 200, 25);
	ob.set_level(Side::ASK, 200, 7); // absolute, not a delta
	EXPECT_EQ(ob.volume_at_price(200, Side::ASK), 7);
}

TEST(OrderBook, SetLevelZeroRemovesLevel) {
	order_book ob;
	ob.set_level(Side::BID, 100, 25);
	ob.set_level(Side::BID, 100, 0); // qty 0 == remove this price
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 0);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, SetLevelZeroOnMissingPriceIsNoOp) {
	order_book ob;
	ob.set_level(Side::BID, 100, 0); // nothing to remove
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, SetLevelKeepsSidesSortedAcrossManyLevels) {
	order_book ob;
	// Insert out of order; best bid_ must stay highest, best ask lowest.
	ob.set_level(Side::BID, 100, 5);
	ob.set_level(Side::BID, 102, 5);
	ob.set_level(Side::BID, 101, 5);
	ob.set_level(Side::ASK, 105, 5);
	ob.set_level(Side::ASK, 103, 5);
	ob.set_level(Side::ASK, 104, 5);

	ASSERT_TRUE(ob.best_bid().has_value());
	ASSERT_TRUE(ob.best_ask().has_value());
	EXPECT_EQ(*ob.best_bid(), 102u);
	EXPECT_EQ(*ob.best_ask(), 103u);

	// Remove the top of each side; the next level becomes best.
	ob.set_level(Side::BID, 102, 0);
	ob.set_level(Side::ASK, 103, 0);
	EXPECT_EQ(*ob.best_bid(), 101u);
	EXPECT_EQ(*ob.best_ask(), 104u);
}

TEST(OrderBook, SetLevelOverAddOrderSeededLevelCollapsesToAbsolute) {
	order_book ob;
	// A level seeded with two resting orders, then taken over by an L2 diff:
	// set_level must overwrite the whole aggregate, not just the FIFO head.
	ob.add_order(Side::BID, 100, 4);
	ob.add_order(Side::BID, 100, 6);
	ASSERT_EQ(ob.volume_at_price(100, Side::BID), 10);

	ob.set_level(Side::BID, 100, 3);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 3);

	ob.set_level(Side::BID, 100, 0);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 0);
	EXPECT_FALSE(ob.best_bid().has_value());
}

// --------------------------------------------------------------------------
// place_order — matching
// --------------------------------------------------------------------------

TEST(OrderBook, CrossingOrderFullyFillsAndEmptiesBook) {
	order_book ob;
	ob.place_order(
		{.id = 1, .side = Side::ASK, .price = 100, .volume = 10}); // rests
	const auto trades = ob.place_order(
		{.id = 2, .side = Side::BID, .price = 100, .volume = 10});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].aggressor, 2u);
	EXPECT_EQ(trades[0].resting, 1u);
	EXPECT_EQ(trades[0].price, 100u); // resting price
	EXPECT_EQ(trades[0].volume, 10);

	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());
}

TEST(OrderBook, PartialCrossRestsRemainderOnAggressorSide) {
	order_book ob;
	ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5});
	const auto trades =
		ob.place_order({.id = 2, .side = Side::BID, .price = 100, .volume = 8});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 5);

	EXPECT_FALSE(ob.best_ask().has_value());          // ask consumed
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), 100u);
	EXPECT_EQ(ob.volume_at_price(100, Side::BID), 3); // remainder rested
}

TEST(OrderBook, MatchingHonoursTimePriority) {
	order_book ob;
	ob.place_order({.id     = 1,
					.side   = Side::ASK,
					.price  = 100,
					.volume = 5}); // first in lockfree
	ob.place_order(
		{.id = 2, .side = Side::ASK, .price = 100, .volume = 5}); // second

	const auto trades =
		ob.place_order({.id = 3, .side = Side::BID, .price = 100, .volume = 5});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].resting, 1u);                 // oldest fills first
	EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 5); // order 2 remains
}

TEST(OrderBook, CrossingSweepsMultipleLevelsUpToLimit) {
	order_book ob;
	ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5});
	ob.place_order({.id = 2, .side = Side::ASK, .price = 101, .volume = 5});
	ob.place_order({.id = 3, .side = Side::ASK, .price = 102, .volume = 5});

	const auto trades =
		ob.place_order({.id = 4, .side = Side::BID, .price = 101, .volume = 8});

	ASSERT_EQ(trades.size(), 2u);
	EXPECT_EQ(trades[0].price, 100u);
	EXPECT_EQ(trades[0].volume, 5);
	EXPECT_EQ(trades[1].price, 101u);
	EXPECT_EQ(trades[1].volume, 3);

	ASSERT_TRUE(ob.best_ask().has_value());
	EXPECT_EQ(*ob.best_ask(), 101u);                  // 100 cleared
	EXPECT_EQ(ob.volume_at_price(101, Side::ASK), 2); // partially filled
	EXPECT_FALSE(ob.best_bid().has_value());          // aggressor fully filled
}

// --------------------------------------------------------------------------
// Order types
// --------------------------------------------------------------------------

TEST(OrderBook, ImmediateOrCancelDropsRemainder) {
	order_book ob;
	ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5});
	const auto trades =
		ob.place_order({.id     = 2,
						.side   = Side::BID,
						.price  = 100,
						.volume = 8,
						.type   = OrderType::IMMEDIATE_OR_CANCEL});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 5);
	EXPECT_FALSE(ob.best_bid().has_value()); // remainder not rested
}

TEST(OrderBook, FillOrKillKilledWhenLiquidityInsufficient) {
	order_book ob;
	ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5});
	const auto trades = ob.place_order({.id     = 2,
										.side   = Side::BID,
										.price  = 100,
										.volume = 8,
										.type   = OrderType::FILL_OR_KILL});

	EXPECT_TRUE(trades.empty());                      // nothing executed
	EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 5); // book untouched
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, FillOrKillExecutesWhenLiquiditySufficient) {
	order_book ob;
	ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 10});
	const auto trades = ob.place_order({.id     = 2,
										.side   = Side::BID,
										.price  = 100,
										.volume = 8,
										.type   = OrderType::FILL_OR_KILL});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 8);
	EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 2); // resting remainder
}
