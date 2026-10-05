// Formatters for what the book holds: a print, a level, and the book itself.

#include "order_book.hpp"
#include "order_book/format.hpp"
#include "orders/types.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

using namespace exchange;
using exchange::side_t;
using exchange::engine::order_book;
using exchange::engine::price_level;
using exchange::engine::trade;
using exchange::engine::orders::order;
using exchange::engine::orders::time_in_force_instruction;

namespace {

TEST(OrderBookFormat, TradeNamesBothSidesOfTheExecution) {
	// No identity: a trade built by hand carries none, so the formatter prints
	// none. The aggressor's side has no "unassigned" value and always prints.
	EXPECT_EQ(fmt::format("{}", trade{1, 2, at_tick(100), 10 * units::lot}),
			  "trade[bid aggressor=1 hit=2 @100 x 10]");
}

TEST(OrderBookFormat, TradePrintsTheIdentityABookGaveIt) {
	EXPECT_EQ(fmt::format("{}",
						  trade{.aggressor      = 1,
								.resting        = 2,
								.price          = at_tick(100),
								.volume         = 10 * units::lot,
								.id             = 7,
								.sequence       = 42,
								.timestamp      = 1'700'000'000'000'000'000,
								.aggressor_side = exchange::side_t::ask}),
			  "trade[#7 seq=42 ask aggressor=1 hit=2 @100 x 10 "
			  "at=1700000000000000000]");
}

TEST(OrderBookFormat, LevelAggregatesItsRestingOrders) {
	const order order{.id    = 1,
					  .side  = side_t::bid,
					  .tif   = time_in_force_instruction::GOOD_TILL_CANCELLED,
					  .price = at_tick(100),
					  .qty   = 10 * units::lot,
					  .timestamp = 0};
	// A level's orders are pool nodes, so a bare Level needs a pool to rest
	// anything in; the book owns one in real use.
	exchange::engine::detail::order_pool pool;
	price_level level{.price = at_tick(100), .orders = {}, .ladder = {}};
	level.add_order(pool, order);
	level.add_order(pool, order);
	EXPECT_EQ(fmt::format("{}", level), "Level[@100 x 20, 2 orders]");
}

TEST(OrderBookFormat, EmptyBookNamesBothSidesAndOmitsTheSpread) {
	const order_book book;
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=none ask=none]");
}

TEST(OrderBookFormat, OneSidedBookOmitsTheSpread) {
	order_book book;
	book.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=100 x 10 ask=none]");
}

TEST(OrderBookFormat, TwoSidedBookReportsTheSpread) {
	order_book book;
	book.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	book.add_order(side_t::ask, at_tick(103), 12 * units::lot);
	EXPECT_EQ(fmt::format("{}", book),
			  "order_book[bid=100 x 10 ask=103 x 12 spread=3]");
}

TEST(OrderBookFormat, TopOfBookAggregatesEveryOrderAtTheTouch) {
	order_book book;
	book.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	book.add_order(side_t::bid, at_tick(100), 5 * units::lot);
	book.add_order(side_t::bid, at_tick(100), 2 * units::lot);
	book.add_order(side_t::bid,
				   at_tick(99),
				   1000 * units::lot); // deeper, must not be counted
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=100 x 17 ask=none]");
}

TEST(OrderBookFormat, TopOfBookFollowsTheTouchAsItMoves) {
	order_book book;
	book.add_order(side_t::bid, at_tick(100), 10 * units::lot);
	book.add_order(side_t::bid, at_tick(99), 7 * units::lot);
	ASSERT_EQ(fmt::format("{}", book), "order_book[bid=100 x 10 ask=none]");

	book.delete_order(side_t::bid, at_tick(100), 10 * units::lot);
	EXPECT_EQ(fmt::format("{}", book), "order_book[bid=99 x 7 ask=none]");
}

} // namespace
