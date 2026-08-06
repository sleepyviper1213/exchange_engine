
#include "core/concurrency/affinity/format.hpp"

#include "trading-engine/orders/types.hpp"
#include "market-data/binance/endpoints.hpp"
#include "market-data/format.hpp"
#include "market-data/parser/fixed_point.hpp"
#include "trading-engine/format.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace aff     = exchange::core::concurrency::affinity;
namespace binance = exchange::market_data::binance;
namespace md      = exchange::market_data;

using exchange::side_t;
using exchange::core::util::formattable_enum;
using exchange::engine::price_level;
using exchange::engine::orders::order;
using exchange::engine::order_book;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::engine::Trade;

// Formatters for market data'\''s composite value types.

namespace {

TEST(MarketDataFormat, AggregatedLevelShowsPriceAndSize) {
	md::l2_book book;
	book.set_level(side_t::bid, 15000, 7);
	EXPECT_EQ(fmt::format("{}", book.bid_levels().front()), "@15000 x 7");
}

TEST(MarketDataFormat, WireLevelKeepsNegativeSizesVisible) {
	// Volume is signed on the wire path; a formatter that assumed unsigned
	// would print a huge positive number instead.
	EXPECT_EQ(fmt::format("{}", binance::PriceLevel{15000, -5}), "@15000 x -5");
}

TEST(MarketDataFormat, EmptyBookNamesBothSidesAsNone) {
	const md::l2_book book;
	EXPECT_EQ(fmt::format("{:s}", book),
			  "l2_book[bids=0 asks=0 best none / none]");
}

TEST(MarketDataFormat, BookReportsDepthAndTopOfBook) {
	md::l2_book book;
	book.set_level(side_t::bid, 15000, 7);
	book.set_level(side_t::bid, 14999, 3);
	book.set_level(side_t::ask, 15001, 4);
	EXPECT_EQ(fmt::format("{:s}", book),
			  "l2_book[bids=2 asks=1 best @15000 x 7 / @15001 x 4]");
}

TEST(MarketDataFormat, OneSidedBookNamesOnlyTheMissingSide) {
	md::l2_book book;
	book.set_level(side_t::ask, 15001, 4);
	EXPECT_EQ(fmt::format("{:s}", book),
			  "l2_book[bids=0 asks=1 best none / @15001 x 4]");
}

TEST(MarketDataFormat, EmptyBookLadderIsJustTheHeader) {
	const md::l2_book book;
	EXPECT_EQ(fmt::format("{}", book), "l2_book[bids=0 asks=0]");
}

TEST(MarketDataFormat, BookLadderPrintsEveryLevelBestFirst) {
	md::l2_book book;
	book.set_level(side_t::bid, 14999, 3);
	book.set_level(side_t::bid, 15000, 7);
	book.set_level(side_t::ask, 15001, 4);
	book.set_level(side_t::ask, 15002, 9);
	EXPECT_EQ(fmt::format("{}", book),
			  "l2_book[bids=2 asks=2]"
			  "\n                  @15000 x 7 | @15001 x 4"
			  "\n                  @14999 x 3 | @15002 x 9");
}

TEST(MarketDataFormat, LadderRowCountFollowsTheDeeperSide) {
	// Replaying diffs without a snapshot seed leaves the sides uneven, so the
	// shallower one must not truncate the deeper one. A row with no ask ends at
	// the separator rather than trailing a space.
	md::l2_book book;
	book.set_level(side_t::bid, 15000, 7);
	book.set_level(side_t::bid, 14999, 3);
	book.set_level(side_t::ask, 15001, 4);
	EXPECT_EQ(fmt::format("{}", book),
			  "l2_book[bids=2 asks=1]"
			  "\n                  @15000 x 7 | @15001 x 4"
			  "\n                  @14999 x 3 |");
}

TEST(MarketDataFormat, LadderCapStatesWhatItWithheld) {
	// A silently truncated book reads as a shallow book, which is the one thing
	// a depth printer must never imply.
	md::l2_book book;
	for (exchange::price_t tick = 0; tick < 4; ++tick)
		book.set_level(side_t::bid, 15000 - tick, 1);
	EXPECT_EQ(
		fmt::format("{:.2}", book),
		"l2_book[bids=4 asks=0]"
		"\n                  @15000 x 1 |"
		"\n                  @14999 x 1 |"
		"\n                             | ... 2 deeper level(s) not shown");
}

TEST(MarketDataFormat, LadderCapWiderThanTheBookWithholdsNothing) {
	md::l2_book book;
	book.set_level(side_t::bid, 15000, 7);
	EXPECT_EQ(fmt::format("{:.50}", book),
			  "l2_book[bids=1 asks=0]"
			  "\n                  @15000 x 7 |");
}

TEST(MarketDataFormat, BookLadderScalesToHumanUnits) {
	// l2_book holds scaled integers and no record of the precision that made
	// them, so book_ladder is what turns 7866 back into 78.66.
	md::l2_book book;
	book.set_level(side_t::bid, 7866, 54'233'700'000);
	book.set_level(side_t::ask, 7867, 36'491'200'000);
	EXPECT_EQ(fmt::format("{}", md::book_ladder{&book, 2, 8}),
			  "l2_book[bids=1 asks=1]"
			  "\n       @78.66 x 542.33700000 | @78.67 x 364.91200000");
}

TEST(MarketDataFormat, BookLadderPadsFractionalDigits) {
	// 5 at 8 decimals is 0.00000005, not 0.5 — the zero-padding is the whole
	// point of scaling rather than dividing.
	md::l2_book book;
	book.set_level(side_t::bid, 100, 5);
	EXPECT_EQ(fmt::format("{}", md::book_ladder{&book, 2, 8}),
			  "l2_book[bids=1 asks=0]"
			  "\n          @1.00 x 0.00000005 |");
}

TEST(MarketDataFormat, EndpointsRenderAsTheUrlTheyDenote) {
	EXPECT_EQ(fmt::format("{}", binance::diff_depth_stream("SOLUSDT")),
			  "wss://stream.binance.com:9443/ws/solusdt@depth@100ms");
	EXPECT_EQ(fmt::format("{}", binance::depth_snapshot("SOLUSDT", 100)),
			  "https://api.binance.com/api/v3/depth?symbol=SOLUSDT&limit=100");
}

TEST(MarketDataFormat, SnapshotAndUpdateReportShapeNotLevels) {
	const binance::DepthSnapshot snapshot{42, {{1, 2}}, {{3, 4}, {5, 6}}};
	EXPECT_EQ(fmt::format("{}", snapshot),
			  "DepthSnapshot[lastUpdateId=42 bids=1 asks=2]");

	const binance::DepthUpdate update{111, 1, 5, {{1, 2}}, {}};
	EXPECT_EQ(fmt::format("{}", update), "depthUpdate[U=1 u=5 bids=1 asks=0]");
	EXPECT_EQ(fmt::format("{}", binance::DepthUpdateMeta{111, 1, 5}),
			  "depthUpdate[U=1 u=5]");
}

} // namespace
