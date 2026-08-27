#include "binance_depth.fixture.hpp"

#include "market_data/binance/binance_depth.hpp"
#include "market_data/l2_book.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace exchange::market_data;
using namespace exchange::market_data::binance;
// parse_depth_update - one depthUpdate frame.

namespace {

TEST(ParseDepthUpdate, ParsesIdsTimeAndLevels) {
	const auto up = parse_binance_depth_update(UPDATE_JSON, 2, 2);
	ASSERT_TRUE(up.has_value()) << message(up.error());

	EXPECT_EQ(up->eventTime, 1'571'889'248'277ull);
	EXPECT_EQ(up->firstUpdateId, 390'497'796ull);
	EXPECT_EQ(up->finalUpdateId, 390'497'878ull);

	ASSERT_EQ(up->bids.size(), 2u);
	EXPECT_EQ(up->bids[0].price, 15345u);
	EXPECT_EQ(up->bids[0].qty, 0); // 0-qty removal preserved as absolute 0
	EXPECT_EQ(up->bids[1].price, 15344u);
	EXPECT_EQ(up->bids[1].qty, 550);

	ASSERT_EQ(up->asks.size(), 1u);
	EXPECT_EQ(up->asks[0].price, 15346u);
	EXPECT_EQ(up->asks[0].qty, 800);
}

TEST(ParseDepthUpdate, EmptySidesYieldEmptyLevels) {
	const auto up = parse_binance_depth_update(
		R"({"e":"depthUpdate","E":1,"s":"X","U":1,"u":1,"b":[],"a":[]})",
		2,
		2);
	ASSERT_TRUE(up.has_value()) << message(up.error());
	EXPECT_TRUE(up->bids.empty());
	EXPECT_TRUE(up->asks.empty());
}

TEST(ParseDepthUpdate, RejectsMissingBidsArray) {
	const auto up = parse_binance_depth_update(
		R"({"e":"depthUpdate","E":1,"U":1,"u":1,"a":[]})",
		2,
		2);
	EXPECT_FALSE(up.has_value());
}

TEST(ParseDepthUpdate, RejectsMalformedJson) {
	EXPECT_FALSE(parse_binance_depth_update("{not json", 2, 2).has_value());
}

TEST(ParseDepthUpdate, RejectsNonNumericQty) {
	const auto up = parse_binance_depth_update(
		R"({"E":1,"U":1,"u":1,"b":[["153.45","oops"]],"a":[]})",
		2,
		2);
	EXPECT_FALSE(up.has_value());
}

} // namespace
