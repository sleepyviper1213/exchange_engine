#include "binance_depth.fixture.hpp"

#include "market_data/binance/binance_depth.hpp"
#include "market_data/l2_book.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace exchange::market_data;
using namespace exchange::market_data::binance;
// apply_depth_update - a decoded frame onto an l2_book.

namespace {

TEST(ApplyDepthUpdate, StreamsLevelsAndReturnsMeta) {
	using namespace exchange;
	l2_book book;
	const auto meta = apply_binance_depth_update(book, UPDATE_JSON, 2, 2);
	ASSERT_TRUE(meta.has_value()) << message(meta.error());

	EXPECT_EQ(meta->eventTime, 1'571'889'248'277ull);
	EXPECT_EQ(meta->firstUpdateId, 390'497'796ull);
	EXPECT_EQ(meta->finalUpdateId, 390'497'878ull);

	// b: 153.45@0 (remove), 153.44@5.50; a: 153.46@8.00.
	EXPECT_EQ(book.volume_at_price(15344, side_t::bid), 550);
	EXPECT_EQ(book.volume_at_price(15345, side_t::bid), 0); // 0-qty removed
	EXPECT_EQ(book.volume_at_price(15346, side_t::ask), 800);
	EXPECT_EQ(*book.best_bid(), 15344u);
	EXPECT_EQ(*book.best_ask(), 15346u);
}

TEST(ApplyDepthUpdate, MatchesParseThenApply) {
	using namespace exchange;
	// Streaming and parse-then-apply must leave identical books.
	l2_book streamed;
	ASSERT_TRUE(
		apply_binance_depth_update(streamed, UPDATE_JSON, 2, 2).has_value());

	l2_book applied;
	const auto parsed = parse_binance_depth_update(UPDATE_JSON, 2, 2);
	ASSERT_TRUE(parsed.has_value()) << message(parsed.error());
	apply_depth_update(applied, *parsed);

	EXPECT_EQ(streamed.best_bid(), applied.best_bid());
	EXPECT_EQ(streamed.best_ask(), applied.best_ask());
	EXPECT_EQ(streamed.volume_at_price(15344, side_t::bid),
			  applied.volume_at_price(15344, side_t::bid));
	EXPECT_EQ(streamed.volume_at_price(15346, side_t::ask),
			  applied.volume_at_price(15346, side_t::ask));
}

TEST(ApplyDepthUpdate, RejectsMalformedJson) {
	using namespace exchange;
	l2_book book;
	EXPECT_FALSE(
		apply_binance_depth_update(book, "{not json", 2, 2).has_value());
}

TEST(ApplyDepthUpdate, depth_parserReusesAcrossFrames) {
	using namespace exchange;
	l2_book book;
	depth_parser parser;
	ASSERT_TRUE(parser.apply_update(book, UPDATE_JSON, 2, 2).has_value());

	// A second frame adds a bid and removes the ask; the reused parser must
	// apply it into the same book.
	const auto second = parser.apply_update(
		book,
		R"({"E":2,"U":1,"u":3,"b":[["153.40","1.00"]],"a":[["153.46","0.00"]]})",
		2,
		2);
	ASSERT_TRUE(second.has_value()) << message(second.error());
	EXPECT_EQ(second->finalUpdateId, 3ull);
	EXPECT_EQ(book.volume_at_price(15340, side_t::bid), 100);
	EXPECT_EQ(book.volume_at_price(15346, side_t::ask),
			  0); // removed by 2nd frame
}

} // namespace
