// The fat-finger collar: where its edges are, and what an unknown mark means.
//
// The band is the one rule whose definition depends on the market, so the two
// degenerate cases - no width configured, and no print yet to centre on - are the
// interesting ones. Both must leave the collar open, because refusing every order
// until the first print would stop a session before it started.

#include "pre_trade.fixture.hpp"
#include "risk_management/hooks/pre_trade/price_collar.hpp"

#include <gtest/gtest.h>


namespace {

using namespace exchange;
using namespace exchange::risk;
using namespace exchange::risk::hooks::pre_trade;

TEST(RiskHooksPriceCollar, TheBandAdmitsItsOwnEdgesAndNothingOutside) {
	const price_band band = price_band::around(at_tick(100), 100);

	EXPECT_EQ(collar_breach(band, at_tick(99)), 0U);
	EXPECT_EQ(collar_breach(band, at_tick(101)), 0U);
	EXPECT_NE(collar_breach(band, at_tick(98)), 0U);
	EXPECT_NE(collar_breach(band, at_tick(102)), 0U);
}

TEST(RiskHooksPriceCollar, APriceUnderTheFloorFailsThroughTheWrap) {
	// The two-sided range is *one* unsigned compare: below the floor the
	// subtraction wraps to something enormous and fails the same test that
	// catches a price above the ceiling. A zero price is the extreme of that.
	const price_band band = price_band::around(at_tick(100), 100);
	EXPECT_NE(collar_breach(band, at_tick(0)), 0U);
	EXPECT_NE(collar_breach(band, at_tick(1)), 0U);
}

TEST(RiskHooksPriceCollar, TheFloorIsOneTickEvenForABandWiderThanTheMark) {
	// A price of zero ticks is never admissible, so a very wide band clamps at
	// one rather than wrapping its floor below zero.
	const price_band band = price_band::around(at_tick(100), 100'000);
	EXPECT_EQ(band.low, at_tick(1));
	EXPECT_EQ(collar_breach(band, at_tick(1)), 0U);
	EXPECT_NE(collar_breach(band, at_tick(0)), 0U);
}

TEST(RiskHooksPriceCollar, AnUnknownMarkOrNoWidthLeavesTheCollarOpen) {
	EXPECT_EQ(collar_breach(price_band::around(at_tick(0), 100), at_tick(1'000'000)), 0U);
	EXPECT_EQ(collar_breach(price_band::around(at_tick(100), 0), at_tick(1'000'000)), 0U);
	// And a default band admits everything, which is what a gate holds before
	// its first reference price arrives.
	EXPECT_EQ(collar_breach(price_band{}, at_tick(1'000'000)), 0U);
}

} // namespace
