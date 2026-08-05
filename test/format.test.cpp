
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
using exchange::engine::price_Level;
using exchange::engine::orders::order;
using exchange::engine::order_book;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::engine::Trade;
using exchange::market_data::parser::parse_error;

namespace {

// --------------------------------------------------------------------------
// format_as — enums format as their string stand-in
// --------------------------------------------------------------------------

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

// --------------------------------------------------------------------------
// Uniformity: every enum declared through the EXCHANGE_ENUM_* helpers gets the
// same hook from the same macro, so these hold for all of them by construction
// rather than by five hand-written copies agreeing with each other.
// --------------------------------------------------------------------------

static_assert(formattable_enum<side_t>);
static_assert(formattable_enum<order_type>);
static_assert(formattable_enum<time_in_force_instruction>);
static_assert(formattable_enum<binance::depth_error>);
static_assert(formattable_enum<binance::depth_speed>);
static_assert(formattable_enum<parse_error>);

// The concept must actually discriminate — an enum with no hook must not match,
// or the assertions above prove nothing.
enum class unhooked_enum : std::uint8_t { a, b };
static_assert(!formattable_enum<unhooked_enum>);
static_assert(!formattable_enum<int>);

/// @brief Round-trip every project enum through the uniform conversions.
template <formattable_enum E>
void expect_uniform(E value, std::string_view expected) {
	// The accessor is the view, fmt::to_string is the owned copy, and "{}" is
	// the in-place render — three spellings, one text.
	EXPECT_EQ(format_as(value), expected);
	EXPECT_EQ(fmt::to_string(value), expected);
	EXPECT_EQ(fmt::format("{}", value), expected);
	// std::string conversion goes through fmt, not a per-enum helper.
	static_assert(std::is_same_v<decltype(fmt::to_string(value)), std::string>);
}

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

// --------------------------------------------------------------------------
// market-data records
// --------------------------------------------------------------------------

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

// "{:s}" is the one-line summary a log wants; "{}" is the full ladder, on the
// grounds that printing a book means wanting to see the book.

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

// --------------------------------------------------------------------------
// depth_parse_error — the formatter is the single definition of this text,
// and binance::message() is defined in terms of it. All four shapes.
// --------------------------------------------------------------------------

TEST(DepthParseErrorFormat, CategoryAloneWhenThereIsNoContextOrLine) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   {},
										   0};
	EXPECT_EQ(fmt::format("{}", error), "invalid number");
}

TEST(DepthParseErrorFormat, ContextPrefixesTheCategory) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   "b[0]",
										   0};
	EXPECT_EQ(fmt::format("{}", error), "b[0]: invalid number");
}

TEST(DepthParseErrorFormat, LinePrefixesTheCategory) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   {},
										   7};
	EXPECT_EQ(fmt::format("{}", error), "line 7: invalid number");
}

TEST(DepthParseErrorFormat, LineThenContextThenCategory) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   "b[0]",
										   7};
	EXPECT_EQ(fmt::format("{}", error), "line 7: b[0]: invalid number");
}

// --------------------------------------------------------------------------
// trading-engine records
// --------------------------------------------------------------------------

TEST(TradingEngineFormat, OrderShowsIdSideSizeAndPolicy) {
	const order order{.id    = 7,
					  .side  = side_t::bid,
					  .tif   = time_in_force_instruction::IMMEDIATE_OR_CANCEL,
					  .price = 100,
					  .qty   = 10,
					  .timestamp = 0};
	EXPECT_EQ(fmt::format("{}", order),
			  "Order[id=7 bid 100 x 10 LIMIT IMMEDIATE_OR_CANCEL]");
	// "c" is the default spelled out, so it must render identically.
	EXPECT_EQ(fmt::format("{:c}", order), fmt::format("{}", order));
}

// The compact form omits what carries no information, so a field it does print
// is a field that was actually set.
TEST(TradingEngineFormat, CompactOrderOmitsTheAbsentTriggerAndTimestamp) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	const auto text = fmt::format("{}", plain);
	EXPECT_EQ(text, "Order[id=7 bid 100 x 10 LIMIT GOOD_TILL_CANCELLED]");
	EXPECT_EQ(text.find("stop"), std::string::npos);
	EXPECT_EQ(text.find("ts="), std::string::npos);
}

TEST(TradingEngineFormat, CompactOrderShowsATriggerAndTimestampWhenSet) {
	const order stop{.id         = 7,
					 .side       = side_t::ask,
					 .type       = order_type::STOP,
					 .price      = 100,
					 .stop_price = 105,
					 .qty        = 10,
					 .timestamp  = 1234};
	EXPECT_EQ(
		fmt::format("{}", stop),
		"Order[id=7 ask 100 stop=105 x 10 STOP GOOD_TILL_CANCELLED ts=1234]");
}

// Verbose has a fixed shape: every field, named, present or not — so two dumps
// can be diffed, and "absent" is distinguishable from "omitted".
TEST(TradingEngineFormat, VerboseOrderPrintsEveryFieldIncludingTheEmptyOnes) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	EXPECT_EQ(fmt::format("{:v}", plain),
			  "Order[id=7 side=bid price=100 stop_price=0 qty=10 type=LIMIT"
			  " tif=GOOD_TILL_CANCELLED timestamp=0]");
}

TEST(TradingEngineFormat, VerboseOrderKeepsItsShapeWhenFieldsAreSet) {
	const order stop{.id         = 7,
					 .side       = side_t::ask,
					 .type       = order_type::STOP,
					 .tif        = time_in_force_instruction::FILL_OR_KILL,
					 .price      = 100,
					 .stop_price = 105,
					 .qty        = 10,
					 .timestamp  = 1234};
	EXPECT_EQ(fmt::format("{:v}", stop),
			  "Order[id=7 side=ask price=100 stop_price=105 qty=10 type=STOP"
			  " tif=FILL_OR_KILL timestamp=1234]");
}

// The mode letter leads, and everything after it is an ordinary spec, so both
// features compose instead of one excluding the other. The fill must be written
// out — see the next test for why.
TEST(TradingEngineFormat, OrderModeComposesWithFillAlignAndWidth) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	const auto compact = fmt::format("{}", plain);
	const auto verbose = fmt::format("{:v}", plain);

	EXPECT_EQ(fmt::format("{:c >60}", plain),
			  std::string(60 - compact.size(), ' ') + compact);
	EXPECT_EQ(fmt::format("[{:v*<140}]", plain),
			  "[" + verbose + std::string(140 - verbose.size(), '*') + "]");
}

// A leading 'v' or 'c' is only a mode when no alignment follows it, because a
// character before an alignment is fmt's fill. The pre-existing meaning wins,
// so nothing that used to format one way quietly changes.
TEST(TradingEngineFormat, AModeLetterFollowedByAnAlignmentIsStillAFill) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	const auto compact = fmt::format("{}", plain);

	// 'v' is the fill and '<' the alignment, so this stays compact, v-padded —
	// it does NOT select verbose.
	EXPECT_EQ(fmt::format("{:v<60}", plain),
			  compact + std::string(60 - compact.size(), 'v'));
	// Likewise 'c' here pads rather than selecting compact; the result is the
	// default rendering, which happens to be compact anyway.
	EXPECT_EQ(fmt::format("{:c>60}", plain),
			  std::string(60 - compact.size(), 'c') + compact);
}

TEST(TradingEngineFormat, TradeNamesBothSidesOfTheExecution) {
	EXPECT_EQ(fmt::format("{}", Trade{1, 2, 100, 10}),
			  "Trade[aggressor=1 hit=2 @100 x 10]");
}

TEST(TradingEngineFormat, LevelAggregatesItsRestingOrders) {
	const order order{.id    = 1,
					  .side  = side_t::bid,
					  .tif   = time_in_force_instruction::GOOD_TILL_CANCELLED,
					  .price = 100,
					  .qty   = 10,
					  .timestamp = 0};
	// A level's orders are pool nodes, so a bare Level needs a pool to rest
	// anything in; the book owns one in real use.
	exchange::engine::detail::order_pool pool;
	price_Level level{100, {}};
	level.add_order(pool, order);
	level.add_order(pool, order);
	EXPECT_EQ(fmt::format("{}", level), "Level[@100 x 20, 2 orders]");
}

TEST(TradingEngineFormat, EmptyBookNamesBothSidesAndOmitsTheSpread) {
	const order_book book;
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=none ask=none]");
}

TEST(TradingEngineFormat, OneSidedBookOmitsTheSpread) {
	order_book book;
	book.add_order(side_t::bid, 100, 10);
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=100 x 10 ask=none]");
}

TEST(TradingEngineFormat, TwoSidedBookReportsTheSpread) {
	order_book book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::ask, 103, 12);
	EXPECT_EQ(fmt::format("{}", book),
			  "order_book[bid=100 x 10 ask=103 x 12 spread=3]");
}

// The size is the whole level, not the order that happens to be at its front —
// a touch backed by three orders is as deep as their sum, and a line that said
// otherwise would understate the quote.
TEST(TradingEngineFormat, TopOfBookAggregatesEveryOrderAtTheTouch) {
	order_book book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::bid, 100, 5);
	book.add_order(side_t::bid, 100, 2);
	book.add_order(side_t::bid, 99, 1000); // deeper, must not be counted
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=100 x 17 ask=none]");
}

// Draining the touch moves it to the next level, size and all.
TEST(TradingEngineFormat, TopOfBookFollowsTheTouchAsItMoves) {
	order_book book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::bid, 99, 7);
	ASSERT_EQ(fmt::format("{}", book), "order_book[bid=100 x 10 ask=none]");

	book.delete_order(side_t::bid, 100, 10);
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=99 x 7 ask=none]");
}

// --------------------------------------------------------------------------
// topology
// --------------------------------------------------------------------------

TEST(TopologyFormat, SummarisesLogicalPhysicalSmtAndLlc) {
	// Two physical cores, two SMT siblings each, one shared last-level cache.
	aff::Topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo),
			  "Topology[4 logical / 2 physical cores, SMT, 1 LLC]");
}

TEST(TopologyFormat, PluralisesTheCacheCount) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {{0, 2}, {1, 3}});
	EXPECT_EQ(fmt::format("{}", topo),
			  "Topology[4 logical / 2 physical cores, SMT, 2 LLCs]");
}

TEST(TopologyFormat, NamesTheAbsenceOfSmt) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo),
			  "Topology[2 logical / 2 physical cores, no SMT, 1 LLC]");
}

TEST(TopologyFormat, CoreNamesItsCpuPhysicalCoreCacheAndSiblingRole) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	// cores is sorted by CoreId, so [0] and [2] are the two of physical core 0.
	EXPECT_EQ(fmt::format("{}", topo.cores[0]),
			  "Core[cpu=0 core=0 llc=0 primary]");
	EXPECT_EQ(fmt::format("{}", topo.cores[2]),
			  "Core[cpu=2 core=0 llc=0 sibling]");
}

TEST(TopologyFormat, CoreVectorPrintsElementWiseThroughRanges) {
	// No formatter<vector<Core>> is written by hand — fmt/ranges.h composes it
	// from formatter<Core>, which is why Core needs one at all.
	aff::Topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo.cores),
			  "[Core[cpu=0 core=0 llc=0 primary], "
			  "Core[cpu=1 core=1 llc=0 primary]]");
}

// --------------------------------------------------------------------------
// nested_formatter contract — fill/align/width apply to the whole record.
// A formatter that wrote straight to ctx.out() would pass every test above
// and fail every one of these.
// --------------------------------------------------------------------------

TEST(NestedFormatterContract, WidthPadsTheWholeRecord) {
	const Trade trade{1, 2, 100, 10};
	const std::string bare = fmt::format("{}", trade);
	ASSERT_EQ(bare.size(), 34u);
	EXPECT_EQ(fmt::format("{:>46}", trade), std::string(12, ' ') + bare);
	EXPECT_EQ(fmt::format("{:<46}", trade), bare + std::string(12, ' '));
}

TEST(NestedFormatterContract, HonoursACustomFillCharacter) {
	md::l2_book book;
	book.set_level(side_t::bid, 15000, 7);
	const auto &level = book.bid_levels().front();
	EXPECT_EQ(fmt::format("[{:*<14}]", level), "[@15000 x 7****]");
	EXPECT_EQ(fmt::format("[{:>14}]", level), "[    @15000 x 7]");
}

TEST(NestedFormatterContract, WidthNarrowerThanTheRecordDoesNotTruncate) {
	const Trade trade{1, 2, 100, 10};
	EXPECT_EQ(fmt::format("{:>4}", trade), fmt::format("{}", trade));
}

TEST(NestedFormatterContract, AppliesToEveryRecordType) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	const order_book book;
	for (const std::string &padded :
		 {fmt::format("{:>60}", topo), fmt::format("{:>60}", book)})
		EXPECT_EQ(padded.size(), 60u);
}

} // namespace
