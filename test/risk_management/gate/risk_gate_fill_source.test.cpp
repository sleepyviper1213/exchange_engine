#include "gate.fixture.hpp"

#include <gtest/gtest.h>

// Where a gate's position comes from. In a backtest the engine is the market
// and every execution it prints moves the position. Once a venue is the
// market, a print the engine makes against our order is a match the venue
// never made - and booking it is how two one-hour Demo Mode runs each ended
// one 50-lot order away from the venue's own trade history.

namespace {

[[nodiscard]] volume_t fill_source_net(harness &h) {
	return h.positions().snapshot(SYMBOL).net_lots;
}

} // namespace

TEST(RiskGateFillSource, AnEnginePrintMovesThePositionByDefault) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 5 * exchange::units::lot)));

	h.gate().on_trade(trade{.aggressor = 1,
							.resting   = 0,
							.price     = at_tick(100),
							.volume    = 5 * exchange::units::lot,
							.id        = 1});

	EXPECT_EQ(fill_source_net(h), 5 * exchange::units::lot);
	EXPECT_EQ(h.gate().ignored_engine_fills(), 0U);
}

TEST(RiskGateFillSource, OnceAVenueIsTheMarketAnEnginePrintIsCountedNotBooked) {
	harness h;
	h.gate().set_books_engine_fills(false);
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 5 * exchange::units::lot)));

	h.gate().on_trade(trade{.aggressor = 1,
							.resting   = 0,
							.price     = at_tick(100),
							.volume    = 5 * exchange::units::lot,
							.id        = 1});

	EXPECT_EQ(fill_source_net(h), 0 * exchange::units::lot);
	EXPECT_EQ(h.gate().ignored_engine_fills(), 1U);
}

TEST(RiskGateFillSource, AVenueFillIsBookedWhateverTheSource) {
	harness h;
	h.gate().set_books_engine_fills(false);
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 5 * exchange::units::lot)));

	h.gate().on_venue_fill(1,
						   side_t::bid,
						   at_tick(100),
						   5 * exchange::units::lot);

	EXPECT_EQ(fill_source_net(h), 5 * exchange::units::lot);
}
