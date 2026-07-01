#include "engine/branchless_binary_search.hpp"
#include "engine/order.hpp"
#include "engine/order_book.hpp"
#include <gtest/gtest.h>


// --------------------------------------------------------------------------
// Queries / single-sided helpers
// --------------------------------------------------------------------------

TEST(OrderBook, VolumeAtPriceAggregatesAndReportsZeroForEmpty) {
    OrderBook ob;
    ob.add_order(Side::BID, 100, 10);
    ob.add_order(Side::BID, 100, 5);

    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 15);
    EXPECT_EQ(ob.volume_at_price(99, Side::BID), 0);
    EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 0);
}

TEST(OrderBook, BestBidAskAreNulloptOnEmptyBook) {
    OrderBook ob;
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
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::BID, .price = 100, .volume = 10}); // no opposite -> rests
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 10);

    ob.cancel_order(1);
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 0);
    EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, CancelUnknownIdIsNoOp) {
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::BID, .price = 100, .volume = 10});
    ob.cancel_order(999); // unknown
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 10);
}

TEST(OrderBook, CancelOneOfTwoAtSameLevelKeepsTheOther) {
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::BID, .price = 100, .volume = 10});
    ob.place_order({.id = 2, .side = Side::BID, .price = 100, .volume = 7});

    ob.cancel_order(1);
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 7);
}

// --------------------------------------------------------------------------
// delete_order — reduce resting volume FIFO-first
// --------------------------------------------------------------------------

TEST(OrderBook, DeletePartialReducesVolume) {
    OrderBook ob;
    ob.add_order(Side::BID, 100, 10);
    ob.delete_order(Side::BID, 100, 4);
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 6);
}

TEST(OrderBook, DeleteFullVolumeRemovesLevel) {
    OrderBook ob;
    ob.add_order(Side::BID, 100, 10);
    ob.delete_order(Side::BID, 100, 10);
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 0);
    EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, DeleteSpanningTwoOrdersDrainsFifoFirst) {
    OrderBook ob;
    ob.add_order(Side::BID, 100, 4); // oldest
    ob.add_order(Side::BID, 100, 6); // newest
    ob.delete_order(Side::BID, 100, 7); // drains first (4) + 3 of the second
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 3);
}

// --------------------------------------------------------------------------
// set_level — absolute L2 diff-feed primitive
// --------------------------------------------------------------------------

TEST(OrderBook, SetLevelCreatesLevel) {
    OrderBook ob;
    ob.set_level(Side::BID, 100, 25);
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 25);
    ASSERT_TRUE(ob.best_bid().has_value());
    EXPECT_EQ(*ob.best_bid(), 100u);
}

TEST(OrderBook, SetLevelOverwritesAbsoluteVolume) {
    OrderBook ob;
    ob.set_level(Side::ASK, 200, 25);
    ob.set_level(Side::ASK, 200, 7); // absolute, not a delta
    EXPECT_EQ(ob.volume_at_price(200, Side::ASK), 7);
}

TEST(OrderBook, SetLevelZeroRemovesLevel) {
    OrderBook ob;
    ob.set_level(Side::BID, 100, 25);
    ob.set_level(Side::BID, 100, 0); // qty 0 == remove this price
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 0);
    EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, SetLevelZeroOnMissingPriceIsNoOp) {
    OrderBook ob;
    ob.set_level(Side::BID, 100, 0); // nothing to remove
    EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, SetLevelKeepsSidesSortedAcrossManyLevels) {
    OrderBook ob;
    // Insert out of order; best bid must stay highest, best ask lowest.
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
    OrderBook ob;
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
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 10}); // rests
    const auto trades =
            ob.place_order({.id = 2, .side = Side::BID, .price = 100, .volume = 10});

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].aggressor, 2u);
    EXPECT_EQ(trades[0].resting, 1u);
    EXPECT_EQ(trades[0].price, 100u); // resting price
    EXPECT_EQ(trades[0].volume, 10);

    EXPECT_FALSE(ob.best_bid().has_value());
    EXPECT_FALSE(ob.best_ask().has_value());
}

TEST(OrderBook, PartialCrossRestsRemainderOnAggressorSide) {
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5});
    const auto trades =
            ob.place_order({.id = 2, .side = Side::BID, .price = 100, .volume = 8});

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].volume, 5);

    EXPECT_FALSE(ob.best_ask().has_value()); // ask consumed
    ASSERT_TRUE(ob.best_bid().has_value());
    EXPECT_EQ(*ob.best_bid(), 100u);
    EXPECT_EQ(ob.volume_at_price(100, Side::BID), 3); // remainder rested
}

TEST(OrderBook, MatchingHonoursTimePriority) {
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5}); // first in queue
    ob.place_order({.id = 2, .side = Side::ASK, .price = 100, .volume = 5}); // second

    const auto trades =
            ob.place_order({.id = 3, .side = Side::BID, .price = 100, .volume = 5});

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].resting, 1u); // oldest fills first
    EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 5); // order 2 remains
}

TEST(OrderBook, CrossingSweepsMultipleLevelsUpToLimit) {
    OrderBook ob;
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
    EXPECT_EQ(*ob.best_ask(), 101u); // 100 cleared
    EXPECT_EQ(ob.volume_at_price(101, Side::ASK), 2); // partially filled
    EXPECT_FALSE(ob.best_bid().has_value()); // aggressor fully filled
}

// --------------------------------------------------------------------------
// Order types
// --------------------------------------------------------------------------

TEST(OrderBook, ImmediateOrCancelDropsRemainder) {
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5});
    const auto trades = ob.place_order({.id = 2,
                                        .side = Side::BID,
                                        .price = 100,
                                        .volume = 8,
                                        .type = OrderType::IMMEDIATE_OR_CANCEL});

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].volume, 5);
    EXPECT_FALSE(ob.best_bid().has_value()); // remainder not rested
}

TEST(OrderBook, FillOrKillKilledWhenLiquidityInsufficient) {
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 5});
    const auto trades = ob.place_order({.id = 2,
                                        .side = Side::BID,
                                        .price = 100,
                                        .volume = 8,
                                        .type = OrderType::FILL_OR_KILL});

    EXPECT_TRUE(trades.empty()); // nothing executed
    EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 5); // book untouched
    EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, FillOrKillExecutesWhenLiquiditySufficient) {
    OrderBook ob;
    ob.place_order({.id = 1, .side = Side::ASK, .price = 100, .volume = 10});
    const auto trades = ob.place_order({.id = 2,
                                        .side = Side::BID,
                                        .price = 100,
                                        .volume = 8,
                                        .type = OrderType::FILL_OR_KILL});

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].volume, 8);
    EXPECT_EQ(ob.volume_at_price(100, Side::ASK), 2); // resting remainder
}
