
#include "core/concurrency/affinity/format.hpp"

#include "core/types.hpp"
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

using exchange::Side;
using exchange::core::util::FormattableEnum;
using exchange::engine::Level;
using exchange::engine::Order;
using exchange::engine::order_book;
using exchange::engine::OrderType;
using exchange::engine::Trade;
using exchange::market_data::parser::parse_error;

namespace {

// --------------------------------------------------------------------------
// format_as — enums format as their string stand-in
// --------------------------------------------------------------------------

TEST(FormatAs, SideFormatsAsItsName) {
	EXPECT_EQ(fmt::format("{}", Side::BID), "BID");
	EXPECT_EQ(fmt::format("{}", Side::ASK), "ASK");
}

TEST(FormatAs, OrderTypeFormatsAsItsEnumeratorName) {
	EXPECT_EQ(fmt::format("{}", OrderType::FILL_OR_KILL), "FILL_OR_KILL");
	EXPECT_EQ(fmt::format("{}", OrderType::GOOD_TILL_CANCELLED),
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
	EXPECT_EQ(fmt::format("[{:>5}]", Side::BID), "[  BID]");
	EXPECT_EQ(fmt::format("[{:*<5}]", Side::ASK), "[ASK**]");
}

// --------------------------------------------------------------------------
// Uniformity: every enum declared through the EXCHANGE_ENUM_* helpers gets the
// same hook from the same macro, so these hold for all of them by construction
// rather than by five hand-written copies agreeing with each other.
// --------------------------------------------------------------------------

static_assert(FormattableEnum<Side>);
static_assert(FormattableEnum<OrderType>);
static_assert(FormattableEnum<binance::depth_error>);
static_assert(FormattableEnum<binance::depth_speed>);
static_assert(FormattableEnum<parse_error>);

// The concept must actually discriminate — an enum with no hook must not match,
// or the assertions above prove nothing.
enum class UnhookedEnum : std::uint8_t { a, b };
static_assert(!FormattableEnum<UnhookedEnum>);
static_assert(!FormattableEnum<int>);

/// @brief Round-trip every project enum through the uniform conversions.
template <FormattableEnum E>
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
	expect_uniform(Side::ASK, "ASK");
	expect_uniform(OrderType::FILL_OR_KILL, "FILL_OR_KILL");
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
	static_assert(format_as(Side::BID) == "BID");
	static_assert(format_as(OrderType::FILL_OR_KILL) == "FILL_OR_KILL");
	SUCCEED();
}

// --------------------------------------------------------------------------
// market-data records
// --------------------------------------------------------------------------

TEST(MarketDataFormat, AggregatedLevelShowsPriceAndSize) {
	md::l2_book book;
	book.set_level(Side::BID, 15000, 7);
	EXPECT_EQ(fmt::format("{}", book.levels(Side::BID).front()), "@15000 x 7");
}

TEST(MarketDataFormat, WireLevelKeepsNegativeSizesVisible) {
	// Volume is signed on the wire path; a formatter that assumed unsigned
	// would print a huge positive number instead.
	EXPECT_EQ(fmt::format("{}", binance::PriceLevel{15000, -5}), "@15000 x -5");
}

TEST(MarketDataFormat, EmptyBookNamesBothSidesAsNone) {
	const md::l2_book book;
	EXPECT_EQ(fmt::format("{}", book),
			  "l2_book[bids=0 asks=0 best none / none]");
}

TEST(MarketDataFormat, BookReportsDepthAndTopOfBook) {
	md::l2_book book;
	book.set_level(Side::BID, 15000, 7);
	book.set_level(Side::BID, 14999, 3);
	book.set_level(Side::ASK, 15001, 4);
	EXPECT_EQ(fmt::format("{}", book),
			  "l2_book[bids=2 asks=1 best @15000 x 7 / @15001 x 4]");
}

TEST(MarketDataFormat, OneSidedBookNamesOnlyTheMissingSide) {
	md::l2_book book;
	book.set_level(Side::ASK, 15001, 4);
	EXPECT_EQ(fmt::format("{}", book),
			  "l2_book[bids=0 asks=1 best none / @15001 x 4]");
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
	const Order order{.id        = 7,
					  .side      = Side::BID,
					  .price     = 100,
					  .volume    = 10,
					  .type      = OrderType::IMMEDIATE_OR_CANCEL,
					  .timestamp = 0};
	EXPECT_EQ(fmt::format("{}", order),
			  "Order[id=7 BID 100 x 10 IMMEDIATE_OR_CANCEL]");
}

TEST(TradingEngineFormat, TradeNamesBothSidesOfTheExecution) {
	EXPECT_EQ(fmt::format("{}", Trade{1, 2, 100, 10}),
			  "Trade[aggressor=1 hit=2 @100 x 10]");
}

TEST(TradingEngineFormat, LevelAggregatesItsRestingOrders) {
	const Order order{.id        = 1,
					  .side      = Side::BID,
					  .price     = 100,
					  .volume    = 10,
					  .type      = OrderType::GOOD_TILL_CANCELLED,
					  .timestamp = 0};
	Level level{100, {}};
	level.add_order(order);
	level.add_order(order);
	EXPECT_EQ(fmt::format("{}", level), "Level[@100 x 20, 2 orders]");
}

TEST(TradingEngineFormat, EmptyBookNamesBothSidesAndOmitsTheSpread) {
	const order_book book;
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=none ask=none]");
}

TEST(TradingEngineFormat, OneSidedBookOmitsTheSpread) {
	order_book book;
	book.add_order(Side::BID, 100, 10);
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=100 ask=none]");
}

TEST(TradingEngineFormat, TwoSidedBookReportsTheSpread) {
	order_book book;
	book.add_order(Side::BID, 100, 10);
	book.add_order(Side::ASK, 103, 10);
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=100 ask=103 spread=3]");
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
	book.set_level(Side::BID, 15000, 7);
	const auto &level = book.levels(Side::BID).front();
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
