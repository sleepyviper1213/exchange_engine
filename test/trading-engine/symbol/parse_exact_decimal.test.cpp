#include "symbol_spec.fixture.hpp"

#include "trading-engine/symbol.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange::test::symbol;

// parse_exact_decimal — strict where market_data's parser is permissive.

namespace {

TEST(ParseExactDecimal, ScalesWholeAndFractionalParts) {
	EXPECT_EQ(parse_exact_decimal("153.45", 2).value(), 15345);
	EXPECT_EQ(parse_exact_decimal("153", 2).value(), 15300);
	EXPECT_EQ(parse_exact_decimal("0.01", 2).value(), 1);
	EXPECT_EQ(parse_exact_decimal("+7.5", 3).value(), 7500); // short fraction pads
	EXPECT_EQ(parse_exact_decimal("42", 0).value(), 42);
}

TEST(ParseExactDecimal, MoreFractionalDigitsThanTheScaleIsRefused) {
	EXPECT_FALSE(parse_exact_decimal("153.456", 2).has_value());
	EXPECT_EQ(parse_exact_decimal("153.456", 2).error(),
			  reject_reason::MALFORMED_DECIMAL);
	// Even when the extra digit is a zero: the listing cannot express it, so
	// accepting it would mean deciding on the client's behalf that it is safe.
	EXPECT_FALSE(parse_exact_decimal("153.450", 2).has_value());
}

TEST(ParseExactDecimal, MalformedInputIsRefused) {
	for (const auto *text : {"", "+", ".", "abc", "1.2.3", "1,5", "-1.00", " 1"})
		EXPECT_FALSE(parse_exact_decimal(text, 2).has_value()) << text;
}

TEST(ParseExactDecimal, OverflowIsRefusedRatherThanWrapped) {
	EXPECT_FALSE(parse_exact_decimal("99999999999999999999", 2).has_value());
	// Fits as digits but not once scaled.
	EXPECT_FALSE(parse_exact_decimal("9223372036854775807", 2).has_value());
}

} // namespace
