#include "binance_depth.fixture.hpp"

#include "market_data/binance/binance_depth.hpp"
#include "market_data/l2_book.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace exchange::market_data;
using namespace exchange::market_data::binance;
// parse_binance_depth_updates - a JSONL capture of many frames.

namespace {

TEST(ParseDepthUpdates, ParsesEachLineInOrderSkippingBlanks) {
	// The blank line between the two frames is skipped by the parser.
	const std::string jsonl =
		fmt::format("{}\n\n{}\n",
					R"({"E":1,"U":1,"u":2,"b":[["153.45","1.00"]],"a":[]})",
					R"({"E":2,"U":3,"u":4,"b":[],"a":[["153.46","2.00"]]})");

	const auto ups = parse_binance_depth_updates(jsonl, 2, 2);
	ASSERT_TRUE(ups.has_value()) << message(ups.error());
	ASSERT_EQ(ups->size(), 2u);

	EXPECT_EQ((*ups)[0].finalUpdateId, 2u);
	ASSERT_EQ((*ups)[0].bids.size(), 1u);
	EXPECT_EQ((*ups)[0].bids[0].qty, 100);

	EXPECT_EQ((*ups)[1].firstUpdateId, 3u);
	ASSERT_EQ((*ups)[1].asks.size(), 1u);
	EXPECT_EQ((*ups)[1].asks[0].price, 15346u);
}

TEST(ParseDepthUpdates, ReportsOffendingLineNumber) {
	const std::string jsonl =
		fmt::format("{}\n{{not json}}\n",
					R"({"E":1,"U":1,"u":2,"b":[],"a":[]})");

	const auto ups = parse_binance_depth_updates(jsonl, 2, 2);
	ASSERT_FALSE(ups.has_value());
	EXPECT_EQ(ups.error().line, 2u) << message(ups.error());
}

} // namespace
