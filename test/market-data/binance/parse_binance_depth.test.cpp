#include "binance_depth.fixture.hpp"

#include "market-data/binance/binance_depth.hpp"
#include "market-data/l2_book.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace exchange::market_data;
using namespace exchange::market_data::binance;
// parse_binance_depth - the REST snapshot payload.

namespace {

TEST(ParseBinanceDepth, ParsesIdAndLevels) {
	const auto snap = parse_binance_depth(SNAPSHOT_JSON, 2, 2);
	ASSERT_TRUE(snap.has_value()) << message(snap.error());

	EXPECT_EQ(snap->lastUpdateId, 123u);
	ASSERT_EQ(snap->bids.size(), 2u);
	ASSERT_EQ(snap->asks.size(), 2u);

	EXPECT_EQ(snap->bids[0].price, 15345u);
	EXPECT_EQ(snap->bids[0].qty, 1000);
	EXPECT_EQ(snap->bids[1].price, 15344u);
	EXPECT_EQ(snap->bids[1].qty, 550);
	EXPECT_EQ(snap->asks[0].price, 15346u);
	EXPECT_EQ(snap->asks[0].qty, 800);
}

TEST(ParseBinanceDepth, EmptyBookYieldsEmptyLevels) {
	const auto snap =
		parse_binance_depth(R"({"lastUpdateId":1,"bids":[],"asks":[]})", 2, 2);
	ASSERT_TRUE(snap.has_value()) << message(snap.error());
	EXPECT_TRUE(snap->bids.empty());
	EXPECT_TRUE(snap->asks.empty());
}

TEST(ParseBinanceDepth, LoadsIntoL2Book) {
	const auto snap = parse_binance_depth(SNAPSHOT_JSON, 2, 2);
	ASSERT_TRUE(snap.has_value()) << message(snap.error());

	l2_book book;
	using namespace exchange;
	for (const auto &level : snap->bids)
		book.set_level(side_t::bid, level.price, level.qty);
	for (const auto &level : snap->asks)
		book.set_level(side_t::ask, level.price, level.qty);

	const auto bid = book.best_bid();
	const auto ask = book.best_ask();
	EXPECT_EQ(*bid, 15345u); // highest bid_
	EXPECT_EQ(*ask, 15346u); // lowest ask
	EXPECT_EQ(book.volume_at_price(15344, side_t::bid), 550);
}

TEST(ParseBinanceDepth, RejectsMissingBids) {
	const auto snap =
		parse_binance_depth(R"({"lastUpdateId":1,"asks":[]})", 2, 2);
	EXPECT_FALSE(snap.has_value());
}

TEST(ParseBinanceDepth, RejectsMalformedJson) {
	const auto snap = parse_binance_depth("{not json", 2, 2);
	EXPECT_FALSE(snap.has_value());
}

TEST(ParseBinanceDepth, RejectsNonNumericPrice) {
	const auto snap = parse_binance_depth(
		R"({"lastUpdateId":1,"bids":[["abc","1.0"]],"asks":[]})",
		2,
		2);
	EXPECT_FALSE(snap.has_value());
}

} // namespace
