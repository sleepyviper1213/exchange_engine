// Position, notional and working quantity, including the one piece of
// arithmetic that is easy to get wrong: gross exposure is the worse side, not
// the sum of both.

#include "orders/types.hpp"
#include "risk_management/hooks/pre_trade/position.hpp"

#include <gtest/gtest.h>

namespace {

using namespace exchange;

using exchange::side_t;
using exchange::symbol_id_t;
using exchange::risk::hooks::pre_trade::position_book;
using exchange::risk::hooks::pre_trade::position_snapshot;

constexpr symbol_id_t POSITION_SYMBOL = 3;

TEST(RiskPositionBook, AFreshBookHoldsNothing) {
	const position_book book{8};
	EXPECT_EQ(book.capacity(), 8U);
	EXPECT_TRUE(book.carries(7));
	EXPECT_FALSE(book.carries(8));

	const position_snapshot flat = book.snapshot(POSITION_SYMBOL);
	EXPECT_EQ(flat.net_lots, 0 * units::lot);
	EXPECT_EQ(flat.net_notional, 0 * (units::tick * units::lot));
	EXPECT_EQ(flat.gross_lots(), 0 * units::lot);
}

TEST(RiskPositionBook, ABuyGoesLongAndCarriesItsNotional) {
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL,
					side_t::bid,
					at_tick(100),
					10 * units::lot);

	const position_snapshot after = book.snapshot(POSITION_SYMBOL);
	EXPECT_EQ(after.net_lots, 10 * units::lot);
	EXPECT_EQ(after.net_notional, 1000 * (units::tick * units::lot));
	EXPECT_EQ(after.bought_lots, 10 * units::lot);
	EXPECT_EQ(after.sold_lots, 0 * units::lot);
}

TEST(RiskPositionBook, ASellGoesShortWithASignedNotional) {
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL, side_t::ask, at_tick(100), 4 * units::lot);

	const position_snapshot after = book.snapshot(POSITION_SYMBOL);
	EXPECT_EQ(after.net_lots, -4 * units::lot);
	EXPECT_EQ(after.net_notional, -400 * (units::tick * units::lot));
	EXPECT_EQ(after.sold_lots, 4 * units::lot);
}

TEST(RiskPositionBook, BuyingThenSellingTheSameSizeNetsFlat) {
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL,
					side_t::bid,
					at_tick(100),
					10 * units::lot);
	book.apply_fill(POSITION_SYMBOL,
					side_t::ask,
					at_tick(110),
					10 * units::lot);

	const position_snapshot after = book.snapshot(POSITION_SYMBOL);
	EXPECT_EQ(after.net_lots, 0 * units::lot);
	// Flat, but ten ticks better off - the notional keeps what the position
	// forgot.
	EXPECT_EQ(after.net_notional, -100 * (units::tick * units::lot));
	EXPECT_EQ(after.bought_lots, 10 * units::lot);
	EXPECT_EQ(after.sold_lots, 10 * units::lot);
}

TEST(RiskPositionBook, WorkingQuantityIsTrackedPerSide) {
	position_book book{8};
	book.add_working(POSITION_SYMBOL, side_t::bid, 30 * units::lot);
	book.add_working(POSITION_SYMBOL, side_t::ask, 12 * units::lot);
	EXPECT_EQ(book.working_lots(POSITION_SYMBOL, side_t::bid), 30 * units::lot);
	EXPECT_EQ(book.working_lots(POSITION_SYMBOL, side_t::ask), 12 * units::lot);

	book.remove_working(POSITION_SYMBOL, side_t::bid, 30 * units::lot);
	EXPECT_EQ(book.working_lots(POSITION_SYMBOL, side_t::bid), 0 * units::lot);
	EXPECT_EQ(book.working_lots(POSITION_SYMBOL, side_t::ask), 12 * units::lot);
}

TEST(RiskPositionBook, TwoListingsDoNotSeeEachOther) {
	position_book book{8};
	book.apply_fill(1, side_t::bid, at_tick(100), 5 * units::lot);
	book.apply_fill(2, side_t::ask, at_tick(200), 7 * units::lot);
	EXPECT_EQ(book.net_lots(1), 5 * units::lot);
	EXPECT_EQ(book.net_lots(2), -7 * units::lot);
}

TEST(RiskPositionBook, GrossExposureTakesTheWorseSideRatherThanTheSum) {
	// Long 10 with 10 working sells is an account *closing*; charging it for 20
	// would penalise reducing risk. The worse side is the buys: 10 + 4 = 14.
	const position_snapshot closing{.net_lots         = 10 * units::lot,
									.working_bid_lots = 4 * units::lot,
									.working_ask_lots = 10 * units::lot};
	EXPECT_EQ(closing.gross_lots(), 14 * units::lot);
}

TEST(RiskPositionBook, GrossExposureCountsAShortGrowingShorter) {
	const position_snapshot shorting{.net_lots         = -10 * units::lot,
									 .working_bid_lots = 1 * units::lot,
									 .working_ask_lots = 5 * units::lot};
	// If the asks fill: -10 - 5 = -15, magnitude 15. If the bids do: -9.
	EXPECT_EQ(shorting.gross_lots(), 15 * units::lot);
}

TEST(RiskPositionBook, GrossExposureOfAFlatAccountIsItsLargerSide) {
	const position_snapshot quoting{.net_lots         = {},
									.working_bid_lots = 3 * units::lot,
									.working_ask_lots = 8 * units::lot};
	EXPECT_EQ(quoting.gross_lots(), 8 * units::lot);
}

TEST(RiskPositionBook, ASelfTradeNetsToZeroBecauseBothSidesAreApplied) {
	// The gate applies a trade once per id it recognises. When both are ours
	// that is twice, in opposite directions, and the answer is right without a
	// special case.
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL, side_t::bid, at_tick(100), 5 * units::lot);
	book.apply_fill(POSITION_SYMBOL, side_t::ask, at_tick(100), 5 * units::lot);
	EXPECT_EQ(book.net_lots(POSITION_SYMBOL), 0 * units::lot);
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).net_notional,
			  0 * (units::tick * units::lot));
}

TEST(RiskPositionBook, ProfitOnAnOpenLongIsTheMarkMinusWhatItCost) {
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL,
					side_t::bid,
					at_tick(100),
					10 * units::lot);
	// Bought 10 at 100; at 110 the position is worth 100 more than it cost.
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(110)),
			  100 * (units::tick * units::lot));
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(100)),
			  0 * (units::tick * units::lot));
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(90)),
			  -100 * (units::tick * units::lot));
}

TEST(RiskPositionBook, ProfitOnAnOpenShortMovesTheOtherWay) {
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL,
					side_t::ask,
					at_tick(100),
					10 * units::lot);
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(90)),
			  100 * (units::tick * units::lot));
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(110)),
			  -100 * (units::tick * units::lot));
}

TEST(RiskPositionBook,
	 ClosingAPositionMakesTheProfitRealisedAndMarkIndependent) {
	// The transition that would need an average-price bucket if the two
	// counters did not already carry it: once flat, the mark stops mattering
	// entirely.
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL,
					side_t::bid,
					at_tick(100),
					10 * units::lot);
	book.apply_fill(POSITION_SYMBOL,
					side_t::ask,
					at_tick(110),
					10 * units::lot);

	ASSERT_EQ(book.net_lots(POSITION_SYMBOL), 0 * units::lot);
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(1)),
			  100 * (units::tick * units::lot));
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(100)),
			  100 * (units::tick * units::lot));
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(100'000)),
			  100 * (units::tick * units::lot));
}

TEST(RiskPositionBook, ProfitCombinesTheRealisedAndTheOpenHalves) {
	position_book book{8};
	book.apply_fill(POSITION_SYMBOL,
					side_t::bid,
					at_tick(100),
					10 * units::lot); // long 10 @ 100
	book.apply_fill(POSITION_SYMBOL,
					side_t::ask,
					at_tick(110),
					4 * units::lot); // realise +40 on 4
	// Six still open. At 105 those are worth +30, so +70 in total.
	EXPECT_EQ(book.net_lots(POSITION_SYMBOL), 6 * units::lot);
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(105)),
			  70 * (units::tick * units::lot));
}

TEST(RiskPositionBook, AFlatBookHasNoProfitAtAnyMark) {
	const position_book book{8};
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(0)),
			  0 * (units::tick * units::lot));
	EXPECT_EQ(book.snapshot(POSITION_SYMBOL).pnl(at_tick(50000)),
			  0 * (units::tick * units::lot));
}

TEST(RiskPositionBook, ResetForgetsOneListingAndLeavesTheOthers) {
	position_book book{8};
	book.apply_fill(1, side_t::bid, at_tick(100), 5 * units::lot);
	book.add_working(1, side_t::ask, 3 * units::lot);
	book.apply_fill(2, side_t::bid, at_tick(100), 9 * units::lot);

	book.reset(1);
	EXPECT_EQ(book.net_lots(1), 0 * units::lot);
	EXPECT_EQ(book.working_lots(1, side_t::ask), 0 * units::lot);
	EXPECT_EQ(book.net_lots(2), 9 * units::lot);
}

} // namespace
