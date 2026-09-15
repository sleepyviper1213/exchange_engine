// Formatters for what the book holds: a print, a level, and the book itself.

#include "EXCHANGE.hpp"
#include "order_book/format.hpp"
#include "orders/types.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

using exchange::side_t;
using exchange::engine::EXCHANGE;
using exchange::engine::price_level;
using exchange::engine::trade;
using exchange::engine::orders::order;
using exchange::engine::orders::time_in_force_instruction;

namespace {

TEST(OrderBookFormat, TradeNamesBothSidesOfTheExecution) {
	EXPECT_EQ(fmt::format("{}", trade{1, 2, 100, 10}),
			  "trade[aggressor=1 hit=2 @100 x 10]");
}

TEST(OrderBookFormat, LevelAggregatesItsRestingOrders) {
	const order order{.id    = 1,
					  .side  = side_t::bid,
					  .tif   = time_in_force_instruction::GOOD_TILL_CANCELLED,
					  .price = 100,
					  .qty   = 10,
					  .timestamp = 0};
	// A level's orders are pool nodes, so a bare Level needs a pool to rest
	// anything in; the book owns one in real use.
	exchange::engine::detail::order_pool pool;
	price_level level{.price = 100, .orders = {}, .ladder = {}};
	level.add_order(pool, order);
	level.add_order(pool, order);
	EXPECT_EQ(fmt::format("{}", level), "Level[@100 x 20, 2 orders]");
}

TEST(OrderBookFormat, EmptyBookNamesBothSidesAndOmitsTheSpread) {
	const EXCHANGE book;
	EXPECT_EQ(fmt::format("{}", book), "EXCHANGE[bid=none ask=none]");
}

TEST(OrderBookFormat, OneSidedBookOmitsTheSpread) {
	EXCHANGE book;
	book.add_order(side_t::bid, 100, 10);
	EXPECT_EQ(fmt::format("{}", book), "EXCHANGE[bid=100 x 10 ask=none]");
}

TEST(OrderBookFormat, TwoSidedBookReportsTheSpread) {
	EXCHANGE book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::ask, 103, 12);
	EXPECT_EQ(fmt::format("{}", book),
			  "EXCHANGE[bid=100 x 10 ask=103 x 12 spread=3]");
}

TEST(OrderBookFormat, TopOfBookAggregatesEveryOrderAtTheTouch) {
	EXCHANGE book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::bid, 100, 5);
	book.add_order(side_t::bid, 100, 2);
	book.add_order(side_t::bid, 99, 1000); // deeper, must not be counted
	EXPECT_EQ(fmt::format("{}", book), "EXCHANGE[bid=100 x 17 ask=none]");
}

TEST(OrderBookFormat, TopOfBookFollowsTheTouchAsItMoves) {
	EXCHANGE book;
	book.add_order(side_t::bid, 100, 10);
	book.add_order(side_t::bid, 99, 7);
	ASSERT_EQ(fmt::format("{}", book), "EXCHANGE[bid=100 x 10 ask=none]");

	book.delete_order(side_t::bid, 100, 10);
	EXPECT_EQ(fmt::format("{}", book), "EXCHANGE[bid=99 x 7 ask=none]");
}

} // namespace
