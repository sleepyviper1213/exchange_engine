#include "transport/rest/request.hpp"
#include "venue/binance/rate_limit.hpp"

#include <gtest/gtest.h>

#include <vector>

// Reading the venue's own rate-limit count off a response header.
//
// The two halves are deliberately in different modules - `venue` knows the
// header's name and the format of its value, `transport` knows how to find a
// header - so this suite exercises them together, which is what a caller does.

using exchange::transport::rest::find_header;
using exchange::transport::rest::header;
using exchange::venue::binance::parse_used_weight;
using exchange::venue::binance::USED_ORDER_COUNT_HEADER;
using exchange::venue::binance::USED_WEIGHT_HEADER;

TEST(BinanceParseUsedWeight, ReadsACountFromTheHeaderValue) {
	EXPECT_EQ(parse_used_weight("1200"), 1200);
	EXPECT_EQ(parse_used_weight("0"), 0);
}

TEST(BinanceParseUsedWeight, AMalformedValueIsNothingRatherThanZero) {
	// Zero would read as "the budget is completely fresh", which licences a
	// full minute of traffic on a header we failed to understand. Nothing
	// leaves the caller's own estimate standing, which errs towards sending
	// less.
	EXPECT_FALSE(parse_used_weight("").has_value());
	EXPECT_FALSE(parse_used_weight("many").has_value());
	EXPECT_FALSE(parse_used_weight("12x").has_value());
	EXPECT_FALSE(parse_used_weight("12.5").has_value());
	EXPECT_FALSE(parse_used_weight(" 12").has_value());
	EXPECT_FALSE(parse_used_weight("-5").has_value());
}

TEST(BinanceParseUsedWeight, TheHeaderIsFoundWhateverItsCapitalisation) {
	// Binance is not consistent about this across endpoints, and HTTP field
	// names are case-insensitive anyway. An exact match is how the budget
	// silently stops being reconciled.
	const std::vector<header> lowercased{
		header{.name = "content-type", .value = "application/json"},
		header{.name = "x-mbx-used-weight-1m", .value = "37"}};

	const auto found = find_header(lowercased, USED_WEIGHT_HEADER);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(parse_used_weight(*found), 37);
}

TEST(BinanceParseUsedWeight, TheOrderCountIsASeparateAllowance) {
	// Two independent limits: an account can exhaust its orders-per-interval
	// while its IP weight budget is barely touched, so reading one and assuming
	// the other is a 429 nobody predicted.
	const std::vector<header> headers{
		header{.name = "X-MBX-USED-WEIGHT-1M", .value = "12"},
		header{.name = "X-MBX-ORDER-COUNT-10S", .value = "48"}};

	EXPECT_EQ(parse_used_weight(*find_header(headers, USED_WEIGHT_HEADER)), 12);
	EXPECT_EQ(parse_used_weight(*find_header(headers, USED_ORDER_COUNT_HEADER)),
			  48);
}

TEST(BinanceParseUsedWeight, AResponseWithoutTheHeaderReconcilesNothing) {
	const std::vector<header> headers{
		header{.name = "content-type", .value = "application/json"}};

	// Not every response carries it, and its absence is not evidence about the
	// budget either way - the caller keeps its estimate.
	EXPECT_FALSE(find_header(headers, USED_WEIGHT_HEADER).has_value());
}
