#include "symbol_spec.fixture.hpp"

#include "symbol.hpp"

#include <gtest/gtest.h>

using namespace exchange;
using namespace exchange::engine;

// symbol_spec's money crossing: tick-lots, which only mean something inside one
// listing, into usdt_t, which means the same thing on every listing - and back,
// for a limit an operator states in money.

namespace {

/// @brief SOLUSDT as Binance publishes it: tick 0.01, step 0.001.
symbol_spec money_solusdt() {
	return symbol_spec{1,
					   "SOLUSDT",
					   2,
					   3,
					   1 * units::scaled_price,
					   1 * units::scaled_size,
					   at_scaled(15000)};
}

/// @brief BTCUSDT as Binance publishes it: tick 0.01, step 0.00001.
symbol_spec money_btcusdt() {
	return symbol_spec{2,
					   "BTCUSDT",
					   2,
					   5,
					   1 * units::scaled_price,
					   1 * units::scaled_size,
					   at_scaled(6'000'000)};
}

constexpr notional_t ONE_TICK_LOT = 1 * (units::tick * units::lot);

TEST(SymbolSpecMoney, ATickLotIsWorthWhatTheListingSays) {
	// 0.01 x 0.001 = 1e-5 USDT, and 0.01 x 0.00001 = 1e-7: the two tick-lots
	// differ by a factor of a hundred, which is the whole reason for the type.
	EXPECT_EQ(money_solusdt().usdt_from(ONE_TICK_LOT), 1000 * units::usdt_e8);
	EXPECT_EQ(money_btcusdt().usdt_from(ONE_TICK_LOT), 10 * units::usdt_e8);
}

TEST(SymbolSpecMoney, ARealNotionalConvertsExactly) {
	// 2.5 SOL at 150.00 is 375 USDT; 0.001 BTC at 60,000.00 is 60 USDT.
	const notional_t sol = notional_of(at_tick(15000), 2500 * units::lot);
	const notional_t btc = notional_of(at_tick(6'000'000), 100 * units::lot);
	EXPECT_EQ(money_solusdt().usdt_from(sol), 375 * units::usdt);
	EXPECT_EQ(money_btcusdt().usdt_from(btc), 60 * units::usdt);
}

TEST(SymbolSpecMoney, TwoListingsAddOnlyAsMoney) {
	const notional_t sol = notional_of(at_tick(15000), 2500 * units::lot);
	const notional_t btc = notional_of(at_tick(6'000'000), 100 * units::lot);
	// As tick-lots the sum compiles and is meaningless: 37,500,000 SOLUSDT
	// tick-lots plus 600,000,000 BTCUSDT ones is not 435 of anything.
	const auto total = *money_solusdt().usdt_from(sol) +
					   *money_btcusdt().usdt_from(btc);
	EXPECT_EQ(total, 435 * units::usdt);
}

TEST(SymbolSpecMoney, ALimitInMoneyRoundsTowardZero) {
	// 2.5 SOLUSDT tick-lots of money is two tick-lots of limit, never three: a
	// converted limit may be tighter than the one stated, never looser.
	const auto spec = money_solusdt();
	EXPECT_EQ(spec.notional_within(2500 * units::usdt_e8), 2 * ONE_TICK_LOT);
	EXPECT_EQ(spec.notional_within(-2500 * units::usdt_e8), -2 * ONE_TICK_LOT);
	EXPECT_EQ(spec.notional_within(999 * units::usdt_e8), notional_t{})
		<< "less than one tick-lot is no tick-lots";
}

TEST(SymbolSpecMoney, WholeTickLotsRoundTrip) {
	const auto spec         = money_btcusdt();
	const notional_t amount = 123'456 * ONE_TICK_LOT;
	EXPECT_EQ(spec.notional_within(*spec.usdt_from(amount)), amount);
}

TEST(SymbolSpecMoney, AFinerListingThatLandsOnTheGridStillConverts) {
	// crypto() reads both sides at 8 decimals - 16 in all, past the money
	// grid - but its 0.01 x 0.00001 tick-lot is still ten of them.
	const auto spec = crypto();
	EXPECT_TRUE(spec.has_usdt_grid());
	EXPECT_EQ(spec.usdt_from(ONE_TICK_LOT), 10 * units::usdt_e8);
}

TEST(SymbolSpecMoney, AListingOffTheGridHasNoMoneyValue) {
	// 1e-8 x 1e-8 is 1e-16 USDT, which no count of 1e-8 can state.
	const symbol_spec dust{3,
						   "DUST",
						   8,
						   8,
						   1 * units::scaled_price,
						   1 * units::scaled_size,
						   at_scaled(100)};
	EXPECT_FALSE(dust.has_usdt_grid());
	EXPECT_FALSE(dust.usdt_from(ONE_TICK_LOT).has_value());
	EXPECT_FALSE(dust.notional_within(1 * units::usdt).has_value());
}

TEST(SymbolSpecMoney, AnAmountTooLargeToStateIsRefused) {
	const auto spec = money_solusdt(); // 1000 per tick-lot
	EXPECT_FALSE(spec.usdt_from(notional_t::max()).has_value());
	EXPECT_FALSE(spec.usdt_from(-notional_t::max()).has_value());
	EXPECT_FALSE(spec.usdt_from(notional_t::min()).has_value());
}

} // namespace
