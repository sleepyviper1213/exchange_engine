#include "order_book.hpp"

#include "core/optimisation/branchless_binary_search.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;

// --------------------------------------------------------------------------
// Queries / single-sided helpers
// --------------------------------------------------------------------------

TEST(OrderBook, VolumeAtPriceAggregatesAndReportsZeroForEmpty) {
	order_book ob;
	ob.add_order(side_t::bid, 100, 10);
	ob.add_order(side_t::bid, 100, 5);

	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 15);
	EXPECT_EQ(ob.volume_at_price(99, side_t::bid), 0);
	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 0);
}

TEST(OrderBook, BestBidAskAreNulloptOnEmptyBook) {
	order_book ob;
	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());

	ob.add_order(side_t::bid, 100, 10);
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), 100u);
	EXPECT_FALSE(ob.best_ask().has_value());
}

// --------------------------------------------------------------------------
// cancel_order (O(1) by id)
// --------------------------------------------------------------------------

TEST(OrderBook, CancelRemovesRestingOrder) {
	order_book ob;
	(void)ob.place_order({.id     = 1,
					.side   = side_t::bid,
					.price  = 100,
					.qty = 10}); // no opposite -> rests
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 10);

	ob.cancel_order(1);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 0);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, CancelUnknownIdIsNoOp) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	ob.cancel_order(999); // unknown
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 10);
}

TEST(OrderBook, CancelOneOfTwoAtSameLevelKeepsTheOther) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	(void)ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 7});

	ob.cancel_order(1);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 7);
}

// --------------------------------------------------------------------------
// delete_order - reduce resting qty FIFO-first
// --------------------------------------------------------------------------

TEST(OrderBook, DeletePartialReducesVolume) {
	order_book ob;
	ob.add_order(side_t::bid, 100, 10);
	ob.delete_order(side_t::bid, 100, 4);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 6);
}

TEST(OrderBook, DeleteFullVolumeRemovesLevel) {
	order_book ob;
	ob.add_order(side_t::bid, 100, 10);
	ob.delete_order(side_t::bid, 100, 10);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 0);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, DeleteSpanningTwoOrdersDrainsFifoFirst) {
	order_book ob;
	ob.add_order(side_t::bid, 100, 4);    // oldest
	ob.add_order(side_t::bid, 100, 6);    // newest
	ob.delete_order(side_t::bid, 100, 7); // drains first (4) + 3 of the second
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 3);
}

// A reduction is the counterpart to add_order and removes only what that put
// there. Draining a client's order would destroy it with no CANCELLED to say so
// - and leave a live entry in whatever record store sits above the book.
TEST(OrderBook, DeleteWalksPastAnIdentifiedOrder) {
	order_book ob;
	std::vector<trade> trades;
	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 5},
				   trades);
	ob.add_order(side_t::bid, 100, 6); // anonymous, behind it in the FIFO

	ob.delete_order(side_t::bid, 100, 11); // asks for everything at the level

	// Only the anonymous 6 went; the client's 5 is untouched.
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 5);

	// And it is still a live order, not an orphaned node: cancelling it works
	// and reports, which is the whole reason the reduction left it alone.
	std::vector<order_outcome> outcomes;
	ob.cancel_order(1, outcomes);
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCELLED);
	EXPECT_EQ(outcomes[0].remaining, 5);
	EXPECT_FALSE(ob.best_bid().has_value());
}

// The identified order is at the head, so a naive FIFO drain would take it
// first. The walk has to step over it and reach the anonymous depth behind.
TEST(OrderBook, DeleteReachesAnonymousDepthBehindAnIdentifiedOrder) {
	order_book ob;
	std::vector<trade> trades;
	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5},
				   trades);
	ob.add_order(side_t::ask, 100, 6);

	ob.delete_order(side_t::ask, 100, 4); // less than the anonymous depth

	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 7); // 5 identified + 2 left
}

// Nothing anonymous to take: the reduction removes what it found, which is
// nothing, and says so by leaving the level alone rather than by failing.
TEST(OrderBook, DeleteOnAWhollyIdentifiedLevelRemovesNothing) {
	order_book ob;
	std::vector<trade> trades;
	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 5},
				   trades);

	ob.delete_order(side_t::bid, 100, 99);

	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 5);
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), 100U);
}

// --------------------------------------------------------------------------
// add_order / delete_order keep the sides sorted
// --------------------------------------------------------------------------

TEST(OrderBook, AnonymousLevelsKeepSidesSortedAcrossManyPrices) {
	order_book ob;
	// Insert out of order; best bid must stay highest, best ask lowest.
	ob.add_order(side_t::bid, 100, 5);
	ob.add_order(side_t::bid, 102, 5);
	ob.add_order(side_t::bid, 101, 5);
	ob.add_order(side_t::ask, 105, 5);
	ob.add_order(side_t::ask, 103, 5);
	ob.add_order(side_t::ask, 104, 5);

	ASSERT_TRUE(ob.best_bid().has_value());
	ASSERT_TRUE(ob.best_ask().has_value());
	EXPECT_EQ(*ob.best_bid(), 102u);
	EXPECT_EQ(*ob.best_ask(), 103u);

	// Drain the top of each side; the next level becomes best.
	ob.delete_order(side_t::bid, 102, 5);
	ob.delete_order(side_t::ask, 103, 5);
	EXPECT_EQ(*ob.best_bid(), 101u);
	EXPECT_EQ(*ob.best_ask(), 104u);
}

// A level fully drained by delete_order is gone, not left at qty 0 - the same
// state an absent price reports.
TEST(OrderBook, DrainingALevelRemovesIt) {
	order_book ob;
	ob.add_order(side_t::bid, 100, 4);
	ob.add_order(side_t::bid, 100, 6);
	ASSERT_EQ(ob.volume_at_price(100, side_t::bid), 10);

	ob.delete_order(side_t::bid, 100, 10);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 0);
	EXPECT_FALSE(ob.best_bid().has_value());
}

// --------------------------------------------------------------------------
// place_order - matching
// --------------------------------------------------------------------------

TEST(OrderBook, CrossingOrderFullyFillsAndEmptiesBook) {
	order_book ob;
	(void)ob.place_order(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10}); // rests
	const auto trades = ob.place_order(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 10});

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
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5});
	const auto trades =
		ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 8});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 5);

	EXPECT_FALSE(ob.best_ask().has_value());          // ask consumed
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), 100u);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 3); // remainder rested
}

TEST(OrderBook, MatchingHonoursTimePriority) {
	order_book ob;
	(void)ob.place_order({.id     = 1,
					.side   = side_t::ask,
					.price  = 100,
					.qty = 5}); // first in lockfree
	(void)ob.place_order(
		{.id = 2, .side = side_t::ask, .price = 100, .qty = 5}); // second

	const auto trades =
		ob.place_order({.id = 3, .side = side_t::bid, .price = 100, .qty = 5});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].resting, 1u);                 // oldest fills first
	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 5); // order 2 remains
}

TEST(OrderBook, CrossingSweepsMultipleLevelsUpToLimit) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5});
	(void)ob.place_order({.id = 2, .side = side_t::ask, .price = 101, .qty = 5});
	(void)ob.place_order({.id = 3, .side = side_t::ask, .price = 102, .qty = 5});

	const auto trades =
		ob.place_order({.id = 4, .side = side_t::bid, .price = 101, .qty = 8});

	ASSERT_EQ(trades.size(), 2u);
	EXPECT_EQ(trades[0].price, 100u);
	EXPECT_EQ(trades[0].volume, 5);
	EXPECT_EQ(trades[1].price, 101u);
	EXPECT_EQ(trades[1].volume, 3);

	ASSERT_TRUE(ob.best_ask().has_value());
	EXPECT_EQ(*ob.best_ask(), 101u);                  // 100 cleared
	EXPECT_EQ(ob.volume_at_price(101, side_t::ask), 2); // partially filled
	EXPECT_FALSE(ob.best_bid().has_value());          // aggressor fully filled
}

// --------------------------------------------------------------------------
// order types
// --------------------------------------------------------------------------

TEST(OrderBook, ImmediateOrCancelDropsRemainder) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::IMMEDIATE_OR_CANCEL,
						.price = 100,
						.qty   = 8});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 5);
	EXPECT_FALSE(ob.best_bid().has_value()); // remainder not rested
}

TEST(OrderBook, FillOrKillKilledWhenLiquidityInsufficient) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::FILL_OR_KILL,
						.price = 100,
						.qty   = 8});

	EXPECT_TRUE(trades.empty());                      // nothing executed
	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 5); // book untouched
	EXPECT_FALSE(ob.best_bid().has_value());
}

// All-or-none differs from fill-or-kill in exactly one place: what happens when
// the book cannot fill it whole. FOK withdraws; AON waits, whole and unfilled.
TEST(OrderBook, AllOrNoneRestsWholeWhenLiquidityInsufficient) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::ALL_OR_NONE,
						.price = 100,
						.qty   = 8});

	EXPECT_TRUE(trades.empty());                        // no partial execution
	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 5); // the ask is untouched
	ASSERT_TRUE(ob.best_bid().has_value());
	// The whole 8 rests, not the 3 that would have been left after a partial.
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 8);
}

TEST(OrderBook, AllOrNoneExecutesWhenLiquiditySufficient) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::ALL_OR_NONE,
						.price = 100,
						.qty   = 8});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 8);
	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 2);
	EXPECT_FALSE(ob.best_bid().has_value()); // filled whole, nothing rests
}

TEST(OrderBook, FillOrKillExecutesWhenLiquiditySufficient) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::FILL_OR_KILL,
						.price = 100,
						.qty   = 8});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 8);
	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 2); // resting remainder
}

// --------------------------------------------------------------------------
// clear
// --------------------------------------------------------------------------

TEST(OrderBook, ClearEmptiesBothSides) {
	order_book ob;
	ob.add_order(side_t::bid, 100, 10);
	ob.add_order(side_t::bid, 99, 7);
	ob.add_order(side_t::ask, 101, 5);

	ob.clear();

	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 0);
	EXPECT_EQ(ob.volume_at_price(99, side_t::bid), 0);
	EXPECT_EQ(ob.volume_at_price(101, side_t::ask), 0);
}

// The index names nodes the sides own. Clearing one without the other would
// leave every entry pointing into a released pool cell, and cancel_order would
// follow it - so a cancel after clear must read as an unknown order, not as a
// cancel of something that no longer exists.
TEST(OrderBook, ClearDropsTheIdIndexSoLaterCancelsAreDeclined) {
	order_book ob;
	std::vector<order_outcome> outcomes;
	(void)ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10});

	ob.clear();
	ob.cancel_order(1, outcomes);

	ASSERT_EQ(outcomes.size(), 1u);
	EXPECT_EQ(outcomes[0].id, 1u);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::UNKNOWN_ORDER);
}

// The point of clear() over a fresh book: the same ids are free again, the pools
// still have their cells, and matching works exactly as it did.
TEST(OrderBook, ClearLeavesTheBookReusable) {
	order_book ob;
	(void)ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5});

	ob.clear();

	// Same id, and it must not collide with the one cleared away.
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5},
				   trades, outcomes);
	ASSERT_EQ(outcomes.size(), 1u);
	EXPECT_EQ(outcomes[0].type, OutcomeType::ACCEPTED);

	const auto crossed =
		ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 5});
	ASSERT_EQ(crossed.size(), 1u);
	EXPECT_EQ(crossed[0].resting, 1u);
	EXPECT_EQ(crossed[0].volume, 5);
	EXPECT_FALSE(ob.best_ask().has_value());
}

TEST(OrderBook, ClearOnAnEmptyBookIsANoOp) {
	order_book ob;
	ob.clear();
	ob.clear();

	EXPECT_FALSE(ob.best_bid().has_value());
	ob.add_order(side_t::bid, 100, 10);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 10);
}
