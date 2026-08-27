#include "market_data/parser/fixed_point.hpp"

#include <gtest/gtest.h>

using namespace exchange::market_data::parser;

// --------------------------------------------------------------------------
// parse_fixed_point - decimal string -> integer scaled by 10^scale
// --------------------------------------------------------------------------

TEST(FixedPoint, IntegerGetsZeroPadded) {
	EXPECT_EQ(parse_fixed_point("153", 2).value(), 15300);
}

TEST(FixedPoint, FractionScaledExactly) {
	EXPECT_EQ(parse_fixed_point("153.45", 2).value(), 15345);
}

TEST(FixedPoint, ExtraFractionDigitsTruncated) {
	EXPECT_EQ(parse_fixed_point("0.999", 2).value(), 99);
}

TEST(FixedPoint, ShortFractionZeroPadded) {
	EXPECT_EQ(parse_fixed_point("5.1", 3).value(), 5100);
}

TEST(FixedPoint, NegativeValue) {
	EXPECT_EQ(parse_fixed_point("-1.5", 2).value(), -150);
}

TEST(FixedPoint, LeadingPlusAccepted) {
	EXPECT_EQ(parse_fixed_point("+7.25", 2).value(), 725);
}

// The 8-fractional-digit Binance form is exactly one SWAR octet - the case the
// wide fold is meant to accelerate.
TEST(FixedPoint, EightDecimalStringHitsSwarPath) {
	EXPECT_EQ(parse_fixed_point("153.45000000", 8).value(), 15'345'000'000LL);
}

// A digit run past 8 (and past 16) exercises the wide fold plus a scalar tail.
TEST(FixedPoint, LongDigitRuns) {
	EXPECT_EQ(parse_fixed_point("1234567890123456", 0).value(),
			  1'234'567'890'123'456LL);
	EXPECT_EQ(parse_fixed_point("123456789012345678", 0).value(),
			  123'456'789'012'345'678LL);
}

TEST(FixedPoint, ParsesInt64Max) {
	EXPECT_EQ(parse_fixed_point("9223372036854775807", 0).value(),
			  9'223'372'036'854'775'807LL);
}

// --------------------------------------------------------------------------
// parse_fixed_point - error taxonomy
// --------------------------------------------------------------------------

TEST(FixedPoint, RejectsEmpty) {
	const auto r = parse_fixed_point("", 2);
	ASSERT_FALSE(r.has_value());
	EXPECT_EQ(r.error(), parse_error::empty);
}

TEST(FixedPoint, RejectsNegativeScale) {
	const auto r = parse_fixed_point("1.0", -1);
	ASSERT_FALSE(r.has_value());
	EXPECT_EQ(r.error(), parse_error::negative_scale);
}

TEST(FixedPoint, RejectsNonNumeric) {
	EXPECT_EQ(parse_fixed_point("abc", 2).error(), parse_error::invalid_char);
	EXPECT_EQ(parse_fixed_point("1.2x", 2).error(), parse_error::invalid_char);
	EXPECT_EQ(parse_fixed_point("1.2.3", 2).error(), parse_error::invalid_char);
}

TEST(FixedPoint, RejectsNoDigits) {
	EXPECT_EQ(parse_fixed_point("-", 2).error(), parse_error::no_digits);
	EXPECT_EQ(parse_fixed_point("+", 2).error(), parse_error::no_digits);
	EXPECT_EQ(parse_fixed_point(".", 2).error(), parse_error::no_digits);
}

TEST(FixedPoint, RejectsOverflow) {
	// Twenty 9s cannot fit in int64.
	const auto r = parse_fixed_point("99999999999999999999", 0);
	ASSERT_FALSE(r.has_value());
	EXPECT_EQ(r.error(), parse_error::overflow);
	// A value within range but scaled past the limit also overflows.
	EXPECT_EQ(parse_fixed_point("9223372036854775807", 1).error(),
			  parse_error::overflow);
}

TEST(FixedPoint, EveryErrorHasAMessage) {
	for (auto e : {parse_error::empty, parse_error::negative_scale,
				   parse_error::invalid_char, parse_error::no_digits,
				   parse_error::overflow})
		EXPECT_FALSE(message(e).empty());
}
