#include "binance/binance_depth.hpp"
#include "engine/order.hpp"
#include "engine/order_book.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace core::engine;
using market_data::binance::parse_binance_depth;
using market_data::binance::parse_binance_depth_update;
using market_data::binance::parse_binance_depth_updates;
using market_data::binance::parse_scaled;

// --------------------------------------------------------------------------
// parse_scaled — decimal string -> integer scaled by 10^decimals
// --------------------------------------------------------------------------

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
    EXPECT_EQ(*v, 15345000000LL);
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

// --------------------------------------------------------------------------
// parse_binance_depth — happy paths
// --------------------------------------------------------------------------

namespace {
constexpr std::string_view kSnapshot =
    R"({"lastUpdateId":123,)"
    R"("bids":[["153.45","10.00"],["153.44","5.50"]],)"
    R"("asks":[["153.46","8.00"],["153.47","2.00"]]})";
}

TEST(ParseBinanceDepth, ParsesIdAndLevels) {
    const auto snap = parse_binance_depth(kSnapshot, 2, 2);
    ASSERT_TRUE(snap.has_value()) << snap.error();

    EXPECT_EQ(snap->lastUpdateId, 123u);
    ASSERT_EQ(snap->bids.size(), 2u);
    ASSERT_EQ(snap->asks.size(), 2u);

    EXPECT_EQ(snap->bids[0].price, 15345u);
    EXPECT_EQ(snap->bids[0].volume, 1000);
    EXPECT_EQ(snap->bids[1].price, 15344u);
    EXPECT_EQ(snap->bids[1].volume, 550);
    EXPECT_EQ(snap->asks[0].price, 15346u);
    EXPECT_EQ(snap->asks[0].volume, 800);
}

TEST(ParseBinanceDepth, EmptyBookYieldsEmptyLevels) {
    const auto snap =
        parse_binance_depth(R"({"lastUpdateId":1,"bids":[],"asks":[]})", 2, 2);
    ASSERT_TRUE(snap.has_value()) << snap.error();
    EXPECT_TRUE(snap->bids.empty());
    EXPECT_TRUE(snap->asks.empty());
}

TEST(ParseBinanceDepth, LoadsIntoOrderBook) {
    const auto snap = parse_binance_depth(kSnapshot, 2, 2);
    ASSERT_TRUE(snap.has_value()) << snap.error();

    OrderBook book;
    for (const auto &level: snap->bids) book.add_order(Side::BID, level.price, level.volume);
    for (const auto &level: snap->asks) book.add_order(Side::ASK, level.price, level.volume);

    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    EXPECT_EQ(*bid, 15345u); // highest bid
    EXPECT_EQ(*ask, 15346u); // lowest ask
    EXPECT_EQ(book.volume_at_price(15344, Side::BID), 550);
}

// --------------------------------------------------------------------------
// parse_binance_depth — unhappy paths
// --------------------------------------------------------------------------

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
        R"({"lastUpdateId":1,"bids":[["abc","1.0"]],"asks":[]})", 2, 2);
    EXPECT_FALSE(snap.has_value());
}

// --------------------------------------------------------------------------
// parse_binance_depth_update — one WebSocket depthUpdate frame
// --------------------------------------------------------------------------

namespace {
// A depthUpdate frame in wire order: e, E, s, U, u, b, a. A qty of "0" means the
// level should be removed.
constexpr std::string_view kUpdate =
    R"({"e":"depthUpdate","E":1571889248277,"s":"SOLUSDT",)"
    R"("U":390497796,"u":390497878,)"
    R"("b":[["153.45","0.00"],["153.44","5.50"]],)"
    R"("a":[["153.46","8.00"]]})";
}

TEST(ParseDepthUpdate, ParsesIdsTimeAndLevels) {
    const auto up = parse_binance_depth_update(kUpdate, 2, 2);
    ASSERT_TRUE(up.has_value()) << up.error();

    EXPECT_EQ(up->eventTime, 1571889248277ull);
    EXPECT_EQ(up->firstUpdateId, 390497796ull);
    EXPECT_EQ(up->finalUpdateId, 390497878ull);

    ASSERT_EQ(up->bids.size(), 2u);
    EXPECT_EQ(up->bids[0].price, 15345u);
    EXPECT_EQ(up->bids[0].volume, 0); // 0-qty removal preserved as absolute 0
    EXPECT_EQ(up->bids[1].price, 15344u);
    EXPECT_EQ(up->bids[1].volume, 550);

    ASSERT_EQ(up->asks.size(), 1u);
    EXPECT_EQ(up->asks[0].price, 15346u);
    EXPECT_EQ(up->asks[0].volume, 800);
}

TEST(ParseDepthUpdate, EmptySidesYieldEmptyLevels) {
    const auto up = parse_binance_depth_update(
        R"({"e":"depthUpdate","E":1,"s":"X","U":1,"u":1,"b":[],"a":[]})", 2, 2);
    ASSERT_TRUE(up.has_value()) << up.error();
    EXPECT_TRUE(up->bids.empty());
    EXPECT_TRUE(up->asks.empty());
}

TEST(ParseDepthUpdate, RejectsMissingBidsArray) {
    const auto up = parse_binance_depth_update(
        R"({"e":"depthUpdate","E":1,"U":1,"u":1,"a":[]})", 2, 2);
    EXPECT_FALSE(up.has_value());
}

TEST(ParseDepthUpdate, RejectsMalformedJson) {
    EXPECT_FALSE(parse_binance_depth_update("{not json", 2, 2).has_value());
}

TEST(ParseDepthUpdate, RejectsNonNumericQty) {
    const auto up = parse_binance_depth_update(
        R"({"E":1,"U":1,"u":1,"b":[["153.45","oops"]],"a":[]})", 2, 2);
    EXPECT_FALSE(up.has_value());
}

// --------------------------------------------------------------------------
// parse_binance_depth_updates — JSONL capture of many frames
// --------------------------------------------------------------------------

TEST(ParseDepthUpdates, ParsesEachLineInOrderSkippingBlanks) {
    const std::string jsonl =
        std::string(R"({"E":1,"U":1,"u":2,"b":[["153.45","1.00"]],"a":[]})") +
        "\n\n" + // a blank line in the middle is skipped
        R"({"E":2,"U":3,"u":4,"b":[],"a":[["153.46","2.00"]]})" + "\n";

    const auto ups = parse_binance_depth_updates(jsonl, 2, 2);
    ASSERT_TRUE(ups.has_value()) << ups.error();
    ASSERT_EQ(ups->size(), 2u);

    EXPECT_EQ((*ups)[0].finalUpdateId, 2u);
    ASSERT_EQ((*ups)[0].bids.size(), 1u);
    EXPECT_EQ((*ups)[0].bids[0].volume, 100);

    EXPECT_EQ((*ups)[1].firstUpdateId, 3u);
    ASSERT_EQ((*ups)[1].asks.size(), 1u);
    EXPECT_EQ((*ups)[1].asks[0].price, 15346u);
}

TEST(ParseDepthUpdates, ReportsOffendingLineNumber) {
    const std::string jsonl =
        std::string(R"({"E":1,"U":1,"u":2,"b":[],"a":[]})") + "\n" +
        "{not json}\n";

    const auto ups = parse_binance_depth_updates(jsonl, 2, 2);
    ASSERT_FALSE(ups.has_value());
    EXPECT_NE(ups.error().find("line 2"), std::string::npos) << ups.error();
}
