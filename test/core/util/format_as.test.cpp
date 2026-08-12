#include "enum_format.fixture.hpp"

#include <gtest/gtest.h>

// format_as — enums format as their string stand-in.

namespace {

TEST(FormatAs, SideFormatsAsItsName) {
	EXPECT_EQ(fmt::format("{}", side_t::bid), "bid");
	EXPECT_EQ(fmt::format("{}", side_t::ask), "ask");
}

TEST(FormatAs, OrderTypeFormatsAsItsEnumeratorName) {
	EXPECT_EQ(fmt::format("{}", order_type::LIMIT), "LIMIT");
	EXPECT_EQ(fmt::format("{}", order_type::MARKET), "MARKET");
}

TEST(FormatAs, TimeInForceFormatsAsItsEnumeratorName) {
	EXPECT_EQ(fmt::format("{}", time_in_force_instruction::FILL_OR_KILL),
			  "FILL_OR_KILL");
	EXPECT_EQ(fmt::format("{}", time_in_force_instruction::GOOD_TILL_CANCELLED),
			  "GOOD_TILL_CANCELLED");
}

TEST(FormatAs, ErrorEnumsFormatAsTheirHumanMessage) {
	EXPECT_EQ(fmt::format("{}", binance::depth_error::malformed_level),
			  "level is not a [price, qty] pair");
	EXPECT_EQ(fmt::format("{}", parse_error::overflow), "number out of range");
}

TEST(FormatAs, InheritsTheStringFormatSpecifiers) {
	// The whole point of format_as over a bespoke formatter: fill, align and
	// width come for free because the type formats *as* a string_view.
	EXPECT_EQ(fmt::format("[{:>5}]", side_t::bid), "[  bid]");
	EXPECT_EQ(fmt::format("[{:*<5}]", side_t::ask), "[ask**]");
}

} // namespace
