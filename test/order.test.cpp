#include "trading-engine/order_book/order.hpp"

#include <gtest/gtest.h>

using namespace exchange::engine;
using namespace exchange;

TEST(Order, EqualOrdersCompareEqual) {
    const Order a{.id = 1, .side = side_t::bid, .price = 100, .qty = 10};
    const Order b{.id = 1, .side = side_t::bid, .price = 100, .qty = 10};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(Order, OrdersDifferingInAnyFieldAreUnequal) {
    const Order base{.id = 1, .side = side_t::bid, .price = 100, .qty = 10};
    EXPECT_NE(base, (Order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10}));
    EXPECT_NE(base, (Order{.id = 1, .side = side_t::ask, .price = 100, .qty = 10}));
    EXPECT_NE(base, (Order{.id = 1, .side = side_t::bid, .price = 101, .qty = 10}));
    EXPECT_NE(base, (Order{.id = 1, .side = side_t::bid, .price = 100, .qty = 11}));
}

TEST(Order, EqualityIgnoresNothingButComparesDefaultedType) {
    // type participates in equality (it is a member); defaults match here.
    const Order a{.id = 7, .side = side_t::ask, .price = 50, .qty = 3};
    const Order b{.id = 7, .side = side_t::ask, .price = 50, .qty = 3,
                  .type = OrderType::FILL_OR_KILL};
    EXPECT_NE(a, b);
}

