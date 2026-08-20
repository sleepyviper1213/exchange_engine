// Position, notional and working quantity, including the one piece of
// arithmetic that is easy to get wrong: gross exposure is the worse side, not
// the sum of both.

#include "trading-engine/orders/types.hpp"
#include "risk_management/hooks/pre_trade/position.hpp"

#include <gtest/gtest.h>

namespace {

using exchange::side_t;
using exchange::symbol_id_t;
using exchange::risk::hooks::pre_trade::position_book;
using exchange::risk::hooks::pre_trade::position_snapshot;

constexpr symbol_id_t SYMBOL = 3;

TEST(RiskPositionBook, AFreshBookHoldsNothing) {
	const position_book book{8};
	EXPECT_EQ(book.capacity(), 8U);
	EXPECT_TRUE(book.carries(7));
	EXPECT_FALSE(book.carries(8));

	const position_snapshot flat = book.snapshot(SYMBOL);
	EXPECT_EQ(flat.net_lots, 0);
	EXPECT_EQ(flat.net_notional, 0);
	EXPECT_EQ(flat.gross_lots(), 0);
}

TEST(RiskPositionBook, ABuyGoesLongAndCarriesItsNotional) {
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::bid, 100, 10);

	const position_snapshot after = book.snapshot(SYMBOL);
	EXPECT_EQ(after.net_lots, 10);
	EXPECT_EQ(after.net_notional, 1000);
	EXPECT_EQ(after.bought_lots, 10);
	EXPECT_EQ(after.sold_lots, 0);
}

TEST(RiskPositionBook, ASellGoesShortWithASignedNotional) {
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::ask, 100, 4);

	const position_snapshot after = book.snapshot(SYMBOL);
	EXPECT_EQ(after.net_lots, -4);
	EXPECT_EQ(after.net_notional, -400);
	EXPECT_EQ(after.sold_lots, 4);
}

TEST(RiskPositionBook, BuyingThenSellingTheSameSizeNetsFlat) {
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::bid, 100, 10);
	book.apply_fill(SYMBOL, side_t::ask, 110, 10);

	const position_snapshot after = book.snapshot(SYMBOL);
	EXPECT_EQ(after.net_lots, 0);
	// Flat, but ten ticks better off - the notional keeps what the position
	// forgot.
	EXPECT_EQ(after.net_notional, -100);
	EXPECT_EQ(after.bought_lots, 10);
	EXPECT_EQ(after.sold_lots, 10);
}

TEST(RiskPositionBook, WorkingQuantityIsTrackedPerSide) {
	position_book book{8};
	book.add_working(SYMBOL, side_t::bid, 30);
	book.add_working(SYMBOL, side_t::ask, 12);
	EXPECT_EQ(book.working_lots(SYMBOL, side_t::bid), 30);
	EXPECT_EQ(book.working_lots(SYMBOL, side_t::ask), 12);

	book.remove_working(SYMBOL, side_t::bid, 30);
	EXPECT_EQ(book.working_lots(SYMBOL, side_t::bid), 0);
	EXPECT_EQ(book.working_lots(SYMBOL, side_t::ask), 12);
}

TEST(RiskPositionBook, TwoListingsDoNotSeeEachOther) {
	position_book book{8};
	book.apply_fill(1, side_t::bid, 100, 5);
	book.apply_fill(2, side_t::ask, 200, 7);
	EXPECT_EQ(book.net_lots(1), 5);
	EXPECT_EQ(book.net_lots(2), -7);
}

TEST(RiskPositionBook, GrossExposureTakesTheWorseSideRatherThanTheSum) {
	// Long 10 with 10 working sells is an account *closing*; charging it for 20
	// would penalise reducing risk. The worse side is the buys: 10 + 4 = 14.
	const position_snapshot closing{.net_lots         = 10,
									.working_bid_lots = 4,
									.working_ask_lots = 10};
	EXPECT_EQ(closing.gross_lots(), 14);
}

TEST(RiskPositionBook, GrossExposureCountsAShortGrowingShorter) {
	const position_snapshot shorting{.net_lots         = -10,
									 .working_bid_lots = 1,
									 .working_ask_lots = 5};
	// If the asks fill: -10 - 5 = -15, magnitude 15. If the bids do: -9.
	EXPECT_EQ(shorting.gross_lots(), 15);
}

TEST(RiskPositionBook, GrossExposureOfAFlatAccountIsItsLargerSide) {
	const position_snapshot quoting{.net_lots         = 0,
									.working_bid_lots = 3,
									.working_ask_lots = 8};
	EXPECT_EQ(quoting.gross_lots(), 8);
}

TEST(RiskPositionBook, ASelfTradeNetsToZeroBecauseBothSidesAreApplied) {
	// The gate applies a trade once per id it recognises. When both are ours
	// that is twice, in opposite directions, and the answer is right without a
	// special case.
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::bid, 100, 5);
	book.apply_fill(SYMBOL, side_t::ask, 100, 5);
	EXPECT_EQ(book.net_lots(SYMBOL), 0);
	EXPECT_EQ(book.snapshot(SYMBOL).net_notional, 0);
}

TEST(RiskPositionBook, ProfitOnAnOpenLongIsTheMarkMinusWhatItCost) {
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::bid, 100, 10);
	// Bought 10 at 100; at 110 the position is worth 100 more than it cost.
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(110), 100);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(100), 0);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(90), -100);
}

TEST(RiskPositionBook, ProfitOnAnOpenShortMovesTheOtherWay) {
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::ask, 100, 10);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(90), 100);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(110), -100);
}

TEST(RiskPositionBook, ClosingAPositionMakesTheProfitRealisedAndMarkIndependent) {
	// The transition that would need an average-price bucket if the two counters
	// did not already carry it: once flat, the mark stops mattering entirely.
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::bid, 100, 10);
	book.apply_fill(SYMBOL, side_t::ask, 110, 10);

	ASSERT_EQ(book.net_lots(SYMBOL), 0);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(1), 100);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(100), 100);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(100'000), 100);
}

TEST(RiskPositionBook, ProfitCombinesTheRealisedAndTheOpenHalves) {
	position_book book{8};
	book.apply_fill(SYMBOL, side_t::bid, 100, 10); // long 10 @ 100
	book.apply_fill(SYMBOL, side_t::ask, 110, 4);  // realise +40 on 4
	// Six still open. At 105 those are worth +30, so +70 in total.
	EXPECT_EQ(book.net_lots(SYMBOL), 6);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(105), 70);
}

TEST(RiskPositionBook, AFlatBookHasNoProfitAtAnyMark) {
	const position_book book{8};
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(0), 0);
	EXPECT_EQ(book.snapshot(SYMBOL).pnl(50'000), 0);
}

TEST(RiskPositionBook, ResetForgetsOneListingAndLeavesTheOthers) {
	position_book book{8};
	book.apply_fill(1, side_t::bid, 100, 5);
	book.add_working(1, side_t::ask, 3);
	book.apply_fill(2, side_t::bid, 100, 9);

	book.reset(1);
	EXPECT_EQ(book.net_lots(1), 0);
	EXPECT_EQ(book.working_lots(1, side_t::ask), 0);
	EXPECT_EQ(book.net_lots(2), 9);
}

} // namespace
