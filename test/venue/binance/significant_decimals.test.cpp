#include "venue/binance/exchange_info.hpp"

#include <gtest/gtest.h>

// The rule that decides what scale every price and size in a run is read on.
//
// Binance right-pads every number to eight decimals whatever the instrument, so
// the padding carries no information and the position of the last *non-zero*
// digit is the whole statement about the grid. Getting this wrong is silent:
// parse_fixed_point truncates surplus precision rather than refusing it, so a
// scale chosen too small rounds levels below it to zero and still reports a
// clean parse.

using namespace exchange::venue::binance;

TEST(BinanceSignificantDecimals, PaddingIsNotPrecision) {
	// The three real ones, and the reason this file exists: they are all
	// different, and the flag defaults assumed they were all two.
	EXPECT_EQ(significant_decimals("0.01000000"), 2) << "SOLUSDT/BTCUSDT tick";
	EXPECT_EQ(significant_decimals("0.00100000"), 3) << "SOLUSDT step";
	EXPECT_EQ(significant_decimals("0.00010000"), 4) << "ETHUSDT step";
	EXPECT_EQ(significant_decimals("0.00001000"), 5) << "BTCUSDT step";
}

TEST(BinanceSignificantDecimals, TheTrimmedFormGivesTheSameAnswer) {
	// A venue that stopped padding, or a hand-written fixture, must not change
	// the grid a run is measured on.
	EXPECT_EQ(significant_decimals("0.001"),
			  significant_decimals("0.00100000"));
	EXPECT_EQ(significant_decimals("0.01"), significant_decimals("0.01000000"));
}

TEST(BinanceSignificantDecimals, AnIntegralIncrementNeedsNoDecimals) {
	EXPECT_EQ(significant_decimals("1.00000000"), 0);
	EXPECT_EQ(significant_decimals("1"), 0);
	EXPECT_EQ(significant_decimals("100"), 0)
		<< "a lot size of 100 units is a whole number of them";
}

TEST(BinanceSignificantDecimals, TheLastNonZeroDigitDecidesRatherThanTheFirst) {
	// 0.10500000 is five hundredths coarser than a thousandth but finer than a
	// hundredth, so it needs three. A rule that stopped at the first non-zero
	// digit would answer one and quantise the grid away.
	EXPECT_EQ(significant_decimals("0.10500000"), 3);
}

TEST(BinanceSignificantDecimals, NonsenseIsZeroRatherThanUndefined) {
	// Never throws and never reads past the end - it is noexcept, and it runs
	// on whatever a venue sent. Nothing here is a *valid* increment; the
	// contract is only that each has an answer.
	EXPECT_EQ(significant_decimals(""), 0);
	EXPECT_EQ(significant_decimals("."), 0);
	EXPECT_EQ(significant_decimals("0."), 0);
	EXPECT_EQ(significant_decimals("abc"), 0);
}
