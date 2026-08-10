
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
using exchange::engine::trade;

// Formatters for the trading engine'\''s composite value types.

namespace {

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

TEST(TradingEngineFormat, OrderModeComposesWithFillAlignAndWidth) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	const auto compact = fmt::format("{}", plain);
	const auto verbose = fmt::format("{:v}", plain);

	EXPECT_EQ(fmt::format("{:c >60}", plain),
			  std::string(60 - compact.size(), ' ') + compact);
	EXPECT_EQ(fmt::format("[{:v*<140}]", plain),
			  "[" + verbose + std::string(140 - verbose.size(), '*') + "]");
}

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
	EXPECT_EQ(fmt::format("{}", trade{1, 2, 100, 10}),
			  "trade[aggressor=1 hit=2 @100 x 10]");
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
	price_level level{100, {}};
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

TEST(TradingEngineFormat, TopOfBookAggregatesEveryOrderAtTheTouch) {
	order_book book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::bid, 100, 5);
	book.add_order(side_t::bid, 100, 2);
	book.add_order(side_t::bid, 99, 1000); // deeper, must not be counted
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=100 x 17 ask=none]");
}

TEST(TradingEngineFormat, TopOfBookFollowsTheTouchAsItMoves) {
	order_book book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::bid, 99, 7);
	ASSERT_EQ(fmt::format("{}", book), "order_book[bid=100 x 10 ask=none]");

	book.delete_order(side_t::bid, 100, 10);
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=99 x 7 ask=none]");
}

// --------------------------------------------------------------------------
// order_manager, order_record, order_handle
// --------------------------------------------------------------------------

namespace exec = exchange::engine::execution;

using exchange::engine::OrderStatus;
using exchange::engine::reject_reason;
using exchange::engine::orders::order;

TEST(TradingEngineFormat, AnEmptyStoreReportsItsCapacity) {
	const exec::order_manager orders{64};
	EXPECT_EQ(fmt::format("{}", orders),
			  "order_manager[live=0 retained=0 peak=0/64]");
}

TEST(TradingEngineFormat, AStoreSeparatesLiveRecordsFromRetainedHistory) {
	exec::order_manager orders{64};
	const auto resting = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	const auto done = orders.admit(
		order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(resting.has_value());
	ASSERT_TRUE(done.has_value());
	orders.apply_fill(*done, 10); // terminal, so retained rather than live

	EXPECT_EQ(fmt::format("{}", orders),
			  "order_manager[live=1 retained=1 peak=2/64]");
}

// evicted is the one number here that reports a loss, so it prints only when
// there is one — zero is the healthy case and would be noise on every line.
TEST(TradingEngineFormat, AStoreReportsEvictionsOnlyWhenHistoryWasLost) {
	exec::order_manager orders{1};
	const auto first = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(first.has_value());
	orders.apply_fill(*first, 10);
	ASSERT_EQ(fmt::format("{}", orders),
			  "order_manager[live=0 retained=1 peak=1/1]");

	// The only slot there is, taken back — record 1 is gone for good.
	ASSERT_TRUE(orders
					.admit(order{.id    = 2,
								 .side  = side_t::bid,
								 .price = 100,
								 .qty   = 10})
					.has_value());
	EXPECT_EQ(fmt::format("{}", orders),
			  "order_manager[live=1 retained=0 peak=1/1 evicted=1]");
}

TEST(TradingEngineFormat, ARecordOmitsTheFieldsThatCarryNoInformation) {
	exec::order_manager orders{64};
	const auto handle = orders.admit(
		order{.id = 42, .symbol_id = 7, .side = side_t::ask, .price = 1250,
			  .qty = 30});
	ASSERT_TRUE(handle.has_value());

	// Unattributed, unstamped and still live: no acct, no ts, no reason.
	EXPECT_EQ(fmt::format("{}", *orders.get(*handle)),
			  "OrderRecord[id=42 sym=7 ask @1250 0/30 LIVE]");
}

TEST(TradingEngineFormat, ARecordShowsProgressAgainstTheOrderQuantity) {
	exec::order_manager orders{64};
	const auto handle = orders.admit(order{.id        = 42,
										   .symbol_id = 7,
										   .side      = side_t::ask,
										   .price     = 1250,
										   .qty       = 30,
										   .timestamp = 1700},
									 99);
	ASSERT_TRUE(handle.has_value());
	orders.apply_fill(*handle, 4);

	EXPECT_EQ(fmt::format("{}", *orders.get(*handle)),
			  "OrderRecord[id=42 sym=7 acct=99 ask @1250 4/30 "
			  "PARTIALLY_FILLED ts=1700]");
}

// A cancel needs no excuse, so its reason stays off the line; an engine-driven
// withdrawal owes one, and that is exactly when it appears.
TEST(TradingEngineFormat, ARecordPrintsAReasonOnlyWhenTheOrderEndedWithOne) {
	exec::order_manager orders{64};
	const auto plain = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	const auto caused = orders.admit(
		order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(plain.has_value());
	ASSERT_TRUE(caused.has_value());

	orders.cancel(*plain);
	orders.cancel(*caused, reject_reason::TIME_IN_FORCE);

	EXPECT_EQ(fmt::format("{}", *orders.get(*plain)),
			  "OrderRecord[id=1 sym=0 bid @100 0/10 CANCELLED]");
	EXPECT_EQ(fmt::format("{}", *orders.get(*caused)),
			  "OrderRecord[id=2 sym=0 bid @100 0/10 CANCELLED TIME_IN_FORCE]");
}

// Two handles naming the same slot at different generations are different
// orders; a rendering that showed only the slot would hide exactly that.
TEST(TradingEngineFormat, AHandlePrintsItsGenerationAndSaysWhenItIsNull) {
	exec::order_manager orders{1};
	const auto first = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(first.has_value());
	orders.cancel(*first);
	EXPECT_EQ(fmt::format("{}", *first), "order_handle[slot=0 gen=0]");

	const auto second = orders.admit(
		order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(fmt::format("{}", *second), "order_handle[slot=0 gen=1]");

	EXPECT_EQ(fmt::format("{}", exec::order_handle{}), "order_handle[none]");
}

// Every formatter here derives from nested_formatter, so fill/align/width apply
// to the whole record — the property that makes them usable in a log column.
TEST(TradingEngineFormat, StoreRenderingsHonourFillAlignAndWidth) {
	const exec::order_manager orders{64};
	EXPECT_EQ(fmt::format("{:>45}", orders),
			  "   order_manager[live=0 retained=0 peak=0/64]");
	EXPECT_EQ(fmt::format("{:.<28}", exec::order_handle{}),
			  "order_handle[none]..........");
}

} // namespace
