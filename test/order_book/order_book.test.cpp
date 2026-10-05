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
	ob.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	ob.add_order(side_t::bid, at_tick(100), 5 * units::lot);

	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 15 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(99), side_t::bid), 0 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask), 0 * units::lot);
}

// add_order is the one command path with no validation stage in front of it: a
// PLACE is answered by reject_if_invalid, an ADD runs straight to a level. Its
// terminus is an order_state whose guard against a non-positive quantity is an
// assertion, and an optimised build has no assertions - what would be left is a
// negative value stored raw into a packed 31-bit field, resting an order of
// roughly two billion lots with the cancellation bit already set. So the size
// is checked here rather than asserted, and a size that is not a size rests
// nothing.
TEST(OrderBook, AddOrderRestsNothingForANonPositiveSize) {
	order_book ob;
	ob.add_order(side_t::bid, at_tick(100), 0 * units::lot);
	ob.add_order(side_t::bid, at_tick(100), -5 * units::lot);
	ob.add_order(side_t::ask,
				 at_tick(100),
				 quantity_t::min());

	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask), 0 * units::lot);
	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());

	// And the book is still perfectly usable afterwards - a refused size is not
	// a poisoned level.
	ob.add_order(side_t::bid, at_tick(100), 7 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 7 * units::lot);
}

TEST(OrderBook, BestBidAskAreNulloptOnEmptyBook) {
	order_book ob;
	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());

	ob.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), at_tick(100));
	EXPECT_FALSE(ob.best_ask().has_value());
}

// --------------------------------------------------------------------------
// cancel_order (O(1) by id)
// --------------------------------------------------------------------------

TEST(OrderBook, CancelRemovesRestingOrder) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::bid,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot}); // no opposite -> rests
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 10 * units::lot);

	ob.cancel_order(1);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, CancelUnknownIdIsNoOp) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::bid,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot});
	ob.cancel_order(999); // unknown
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 10 * units::lot);
}

TEST(OrderBook, CancelOneOfTwoAtSameLevelKeepsTheOther) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::bid,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot});
	(void)ob.place_order({.id    = 2,
						  .side  = side_t::bid,
						  .price = at_tick(100),
						  .qty   = 7 * units::lot});

	ob.cancel_order(1);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 7 * units::lot);
}

// --------------------------------------------------------------------------
// delete_order - reduce resting qty FIFO-first
// --------------------------------------------------------------------------

TEST(OrderBook, DeletePartialReducesVolume) {
	order_book ob;
	ob.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	ob.delete_order(side_t::bid, at_tick(100), 4 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 6 * units::lot);
}

TEST(OrderBook, DeleteFullVolumeRemovesLevel) {
	order_book ob;
	ob.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	ob.delete_order(side_t::bid, at_tick(100), 10 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, DeleteSpanningTwoOrdersDrainsFifoFirst) {
	order_book ob;
	ob.add_order(side_t::bid, at_tick(100), 4 * units::lot); // oldest
	ob.add_order(side_t::bid, at_tick(100), 6 * units::lot); // newest
	ob.delete_order(side_t::bid,
					at_tick(100),
					7 * units::lot); // drains first (4) + 3 of the second
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 3 * units::lot);
}

// A reduction is the counterpart to add_order and removes only what that put
// there. Draining a client's order would destroy it with no CANCELLED to say so
// - and leave a live entry in whatever record store sits above the book.
TEST(OrderBook, DeleteWalksPastAnIdentifiedOrder) {
	order_book ob;
	std::vector<trade> trades;
	ob.place_order({.id    = 1,
					.side  = side_t::bid,
					.price = at_tick(100),
					.qty   = 5 * units::lot},
				   trades);
	ob.add_order(side_t::bid,
				 at_tick(100),
				 6 * units::lot); // anonymous, behind it in the FIFO

	ob.delete_order(side_t::bid,
					at_tick(100),
					11 * units::lot); // asks for everything at the level

	// Only the anonymous 6 went; the client's 5 is untouched.
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 5 * units::lot);

	// And it is still a live order, not an orphaned node: cancelling it works
	// and reports, which is the whole reason the reduction left it alone.
	std::vector<order_outcome> outcomes;
	ob.cancel_order(1, outcomes);
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCELLED);
	EXPECT_EQ(outcomes[0].remaining, 5 * units::lot);
	EXPECT_FALSE(ob.best_bid().has_value());
}

// The identified order is at the head, so a naive FIFO drain would take it
// first. The walk has to step over it and reach the anonymous depth behind.
TEST(OrderBook, DeleteReachesAnonymousDepthBehindAnIdentifiedOrder) {
	order_book ob;
	std::vector<trade> trades;
	ob.place_order({.id    = 1,
					.side  = side_t::ask,
					.price = at_tick(100),
					.qty   = 5 * units::lot},
				   trades);
	ob.add_order(side_t::ask, at_tick(100), 6 * units::lot);

	ob.delete_order(side_t::ask,
					at_tick(100),
					4 * units::lot); // less than the anonymous depth

	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask),
			  7 * units::lot);       // 5 identified + 2 left
}

// Nothing anonymous to take: the reduction removes what it found, which is
// nothing, and says so by leaving the level alone rather than by failing.
TEST(OrderBook, DeleteOnAWhollyIdentifiedLevelRemovesNothing) {
	order_book ob;
	std::vector<trade> trades;
	ob.place_order({.id    = 1,
					.side  = side_t::bid,
					.price = at_tick(100),
					.qty   = 5 * units::lot},
				   trades);

	ob.delete_order(side_t::bid, at_tick(100), 99 * units::lot);

	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 5 * units::lot);
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), at_tick(100));
}

// --------------------------------------------------------------------------
// add_order / delete_order keep the sides sorted
// --------------------------------------------------------------------------

TEST(OrderBook, AnonymousLevelsKeepSidesSortedAcrossManyPrices) {
	order_book ob;
	// Insert out of order; best bid must stay highest, best ask lowest.
	ob.add_order(side_t::bid, at_tick(100), 5 * units::lot);
	ob.add_order(side_t::bid, at_tick(102), 5 * units::lot);
	ob.add_order(side_t::bid, at_tick(101), 5 * units::lot);
	ob.add_order(side_t::ask, at_tick(105), 5 * units::lot);
	ob.add_order(side_t::ask, at_tick(103), 5 * units::lot);
	ob.add_order(side_t::ask, at_tick(104), 5 * units::lot);

	ASSERT_TRUE(ob.best_bid().has_value());
	ASSERT_TRUE(ob.best_ask().has_value());
	EXPECT_EQ(*ob.best_bid(), at_tick(102));
	EXPECT_EQ(*ob.best_ask(), at_tick(103));

	// Drain the top of each side; the next level becomes best.
	ob.delete_order(side_t::bid, at_tick(102), 5 * units::lot);
	ob.delete_order(side_t::ask, at_tick(103), 5 * units::lot);
	EXPECT_EQ(*ob.best_bid(), at_tick(101));
	EXPECT_EQ(*ob.best_ask(), at_tick(104));
}

// A level fully drained by delete_order is gone, not left at qty 0 - the same
// state an absent price reports.
TEST(OrderBook, DrainingALevelRemovesIt) {
	order_book ob;
	ob.add_order(side_t::bid, at_tick(100), 4 * units::lot);
	ob.add_order(side_t::bid, at_tick(100), 6 * units::lot);
	ASSERT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 10 * units::lot);

	ob.delete_order(side_t::bid, at_tick(100), 10 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
	EXPECT_FALSE(ob.best_bid().has_value());
}

// --------------------------------------------------------------------------
// place_order - matching
// --------------------------------------------------------------------------

TEST(OrderBook, CrossingOrderFullyFillsAndEmptiesBook) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot}); // rests
	const auto trades = ob.place_order({.id    = 2,
										.side  = side_t::bid,
										.price = at_tick(100),
										.qty   = 10 * units::lot});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].aggressor, 2u);
	EXPECT_EQ(trades[0].resting, 1u);
	EXPECT_EQ(trades[0].price, at_tick(100)); // resting price
	EXPECT_EQ(trades[0].volume, 10 * units::lot);

	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());
}

TEST(OrderBook, PartialCrossRestsRemainderOnAggressorSide) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot});
	const auto trades = ob.place_order({.id    = 2,
										.side  = side_t::bid,
										.price = at_tick(100),
										.qty   = 8 * units::lot});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 5 * units::lot);

	EXPECT_FALSE(ob.best_ask().has_value());          // ask consumed
	ASSERT_TRUE(ob.best_bid().has_value());
	EXPECT_EQ(*ob.best_bid(), at_tick(100));
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid),
			  3 * units::lot); // remainder rested
}

TEST(OrderBook, MatchingHonoursTimePriority) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot}); // first in lockfree
	(void)ob.place_order({.id    = 2,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot}); // second

	const auto trades = ob.place_order({.id    = 3,
										.side  = side_t::bid,
										.price = at_tick(100),
										.qty   = 5 * units::lot});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].resting, 1u);                 // oldest fills first
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask),
			  5 * units::lot);                        // order 2 remains
}

TEST(OrderBook, CrossingSweepsMultipleLevelsUpToLimit) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot});
	(void)ob.place_order({.id    = 2,
						  .side  = side_t::ask,
						  .price = at_tick(101),
						  .qty   = 5 * units::lot});
	(void)ob.place_order({.id    = 3,
						  .side  = side_t::ask,
						  .price = at_tick(102),
						  .qty   = 5 * units::lot});

	const auto trades = ob.place_order({.id    = 4,
										.side  = side_t::bid,
										.price = at_tick(101),
										.qty   = 8 * units::lot});

	ASSERT_EQ(trades.size(), 2u);
	EXPECT_EQ(trades[0].price, at_tick(100));
	EXPECT_EQ(trades[0].volume, 5 * units::lot);
	EXPECT_EQ(trades[1].price, at_tick(101));
	EXPECT_EQ(trades[1].volume, 3 * units::lot);

	ASSERT_TRUE(ob.best_ask().has_value());
	EXPECT_EQ(*ob.best_ask(), at_tick(101));          // 100 cleared
	EXPECT_EQ(ob.volume_at_price(at_tick(101), side_t::ask),
			  2 * units::lot);                        // partially filled
	EXPECT_FALSE(ob.best_bid().has_value());          // aggressor fully filled
}

// --------------------------------------------------------------------------
// order types
// --------------------------------------------------------------------------

TEST(OrderBook, ImmediateOrCancelDropsRemainder) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::IMMEDIATE_OR_CANCEL,
						.price = at_tick(100),
						.qty   = 8 * units::lot});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 5 * units::lot);
	EXPECT_FALSE(ob.best_bid().has_value()); // remainder not rested
}

TEST(OrderBook, FillOrKillKilledWhenLiquidityInsufficient) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::FILL_OR_KILL,
						.price = at_tick(100),
						.qty   = 8 * units::lot});

	EXPECT_TRUE(trades.empty());                      // nothing executed
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask),
			  5 * units::lot);                        // book untouched
	EXPECT_FALSE(ob.best_bid().has_value());
}

// All-or-none is refused at admission, and these three suites pin why rather
// than merely that. The instruction says "fill me whole or leave me resting
// until I am", and the second half is the half this book cannot keep: a resting
// order carries no time-in-force, so an accepted AON becomes an ordinary GTC
// order the next aggressor fills in part - the one outcome it exists to forbid.
// Refusing it is what keeps the book from silently honouring a different
// instruction than the one it was given.
TEST(OrderBook, AllOrNoneIsRejectedWhenLiquidityIsInsufficient) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot});
	ob.place_order({.id    = 2,
					.side  = side_t::bid,
					.tif   = time_in_force_instruction::ALL_OR_NONE,
					.price = at_tick(100),
					.qty   = 8 * units::lot},
				   trades,
				   outcomes);

	EXPECT_TRUE(trades.empty());
	ASSERT_EQ(outcomes.size(), 1u);
	EXPECT_EQ(outcomes[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::UNSUPPORTED_TIME_IN_FORCE);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask),
			  5 * units::lot); // the ask is untouched
	// And nothing rests. This is the assertion the old behaviour failed: it
	// rested the whole 8 as an order nothing could keep whole afterwards.
	EXPECT_FALSE(ob.best_bid().has_value());
}

// Refused even when the book could have filled it whole this instant. The
// instruction is refused, not the outcome - accepting the easy case would mean
// a client's AON sometimes executes and sometimes silently becomes a GTC, with
// the difference decided by liquidity it cannot see.
TEST(OrderBook, AllOrNoneIsRejectedEvenWhenLiquidityWouldCoverIt) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot});
	ob.place_order({.id    = 2,
					.side  = side_t::bid,
					.tif   = time_in_force_instruction::ALL_OR_NONE,
					.price = at_tick(100),
					.qty   = 8 * units::lot},
				   trades,
				   outcomes);

	EXPECT_TRUE(trades.empty());
	ASSERT_EQ(outcomes.size(), 1u);
	EXPECT_EQ(outcomes[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::UNSUPPORTED_TIME_IN_FORCE);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask),
			  10 * units::lot); // untouched
	EXPECT_FALSE(ob.best_bid().has_value());
}

// The anonymous rule holds here as it does for every other refusal: id zero has
// nobody to report to, so the order is dropped without an outcome.
TEST(OrderBook, AllOrNoneUnderTheAnonymousIdReportsNothing) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot});
	ob.place_order({.id    = 0,
					.side  = side_t::bid,
					.tif   = time_in_force_instruction::ALL_OR_NONE,
					.price = at_tick(100),
					.qty   = 8 * units::lot},
				   trades,
				   outcomes);

	EXPECT_TRUE(trades.empty());
	EXPECT_TRUE(outcomes.empty());
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask), 10 * units::lot);
	EXPECT_FALSE(ob.best_bid().has_value());
}

TEST(OrderBook, FillOrKillExecutesWhenLiquiditySufficient) {
	order_book ob;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot});
	const auto trades =
		ob.place_order({.id    = 2,
						.side  = side_t::bid,
						.tif   = time_in_force_instruction::FILL_OR_KILL,
						.price = at_tick(100),
						.qty   = 8 * units::lot});

	ASSERT_EQ(trades.size(), 1u);
	EXPECT_EQ(trades[0].volume, 8 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::ask),
			  2 * units::lot); // resting remainder
}

// --------------------------------------------------------------------------
// clear
// --------------------------------------------------------------------------

TEST(OrderBook, ClearEmptiesBothSides) {
	order_book ob;
	ob.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	ob.add_order(side_t::bid, at_tick(99), 7 * units::lot);
	ob.add_order(side_t::ask, at_tick(101), 5 * units::lot);

	ob.clear();

	EXPECT_FALSE(ob.best_bid().has_value());
	EXPECT_FALSE(ob.best_ask().has_value());
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(99), side_t::bid), 0 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(101), side_t::ask), 0 * units::lot);
}

// The index names nodes the sides own. Clearing one without the other would
// leave every entry pointing into a released pool cell, and cancel_order would
// follow it - so a cancel after clear must read as an unknown order, not as a
// cancel of something that no longer exists.
TEST(OrderBook, ClearDropsTheIdIndexSoLaterCancelsAreDeclined) {
	order_book ob;
	std::vector<order_outcome> outcomes;
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::bid,
						  .price = at_tick(100),
						  .qty   = 10 * units::lot});

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
	(void)ob.place_order({.id    = 1,
						  .side  = side_t::ask,
						  .price = at_tick(100),
						  .qty   = 5 * units::lot});

	ob.clear();

	// Same id, and it must not collide with the one cleared away.
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	ob.place_order({.id    = 1,
					.side  = side_t::ask,
					.price = at_tick(100),
					.qty   = 5 * units::lot},
				   trades,
				   outcomes);
	ASSERT_EQ(outcomes.size(), 1u);
	EXPECT_EQ(outcomes[0].type, OutcomeType::ACCEPTED);

	const auto crossed = ob.place_order({.id    = 2,
										 .side  = side_t::bid,
										 .price = at_tick(100),
										 .qty   = 5 * units::lot});
	ASSERT_EQ(crossed.size(), 1u);
	EXPECT_EQ(crossed[0].resting, 1u);
	EXPECT_EQ(crossed[0].volume, 5 * units::lot);
	EXPECT_FALSE(ob.best_ask().has_value());
}

TEST(OrderBook, ClearOnAnEmptyBookIsANoOp) {
	order_book ob;
	ob.clear();
	ob.clear();

	EXPECT_FALSE(ob.best_bid().has_value());
	ob.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	EXPECT_EQ(ob.volume_at_price(at_tick(100), side_t::bid), 10 * units::lot);
}
