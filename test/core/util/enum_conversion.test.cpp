#include "enum_format.fixture.hpp"

#include <gtest/gtest.h>


// The three spellings of an enum-to-text conversion, held to one answer.

namespace {

TEST(EnumConversion, EveryEnumConvertsTheSameThreeWays) {
	expect_uniform(side_t::ask, "ask");
	expect_uniform(time_in_force_instruction::FILL_OR_KILL, "FILL_OR_KILL");
	expect_uniform(binance::depth_speed::every_1000ms, "1000ms");
	expect_uniform(binance::depth_error::bad_number, "invalid number");
	expect_uniform(parse_error::no_digits, "no digits in number");
}

TEST(EnumConversion, OutOfRangeValueYieldsEmptyRatherThanGarbage) {
	// The generated switch falls through to {} — an out-of-range value must not
	// read past the table or print an integer.
	const auto bogus = static_cast<parse_error>(200);
	EXPECT_TRUE(format_as(bogus).empty());
	EXPECT_EQ(fmt::to_string(bogus), "");
}

TEST(EnumConversion, ConversionIsUsableInAConstantExpression) {
	// format_as is constexpr, so the text is available at compile time even
	// though fmt::to_string is not.
	static_assert(format_as(side_t::bid) == "bid");
	static_assert(format_as(time_in_force_instruction::FILL_OR_KILL) ==
				  "FILL_OR_KILL");
	SUCCEED();
}

} // namespace
