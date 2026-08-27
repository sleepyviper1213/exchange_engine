#include "binance_depth.fixture.hpp"

#include "market_data/binance/binance_depth.hpp"
#include "market_data/l2_book.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace exchange::market_data;
using namespace exchange::market_data::binance;
// parse_scaled - decimal string -> integer scaled by 10^decimals.

namespace {

TEST(ParseScaled, IntegerGetsZeroPadded) {
	const auto v = parse_scaled("153", 2);
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(*v, 15300);
}

TEST(ParseScaled, FractionScaledExactly) {
	const auto v = parse_scaled("153.45", 2);
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(*v, 15345);
}

TEST(ParseScaled, ExtraFractionDigitsTruncated) {
	const auto v = parse_scaled("0.999", 2); // -> 0.99
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(*v, 99);
}

TEST(ParseScaled, ShortFractionZeroPadded) {
	const auto v = parse_scaled("5.1", 3);
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(*v, 5100);
}

TEST(ParseScaled, EightDecimalBinanceString) {
	const auto v = parse_scaled("153.45000000", 8);
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(*v, 15'345'000'000LL);
}

TEST(ParseScaled, NegativeValue) {
	const auto v = parse_scaled("-1.5", 2);
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(*v, -150);
}

TEST(ParseScaled, RejectsEmpty) {
	EXPECT_FALSE(parse_scaled("", 2).has_value());
}

TEST(ParseScaled, RejectsNonNumeric) {
	EXPECT_FALSE(parse_scaled("abc", 2).has_value());
	EXPECT_FALSE(parse_scaled("1.2x", 2).has_value());
	EXPECT_FALSE(parse_scaled("1.2.3", 2).has_value());
}

TEST(ParseScaled, RejectsNegativeDecimals) {
	EXPECT_FALSE(parse_scaled("1.0", -1).has_value());
}

} // namespace
