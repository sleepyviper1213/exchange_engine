#include "trading-engine/order_book/order.hpp"

#include <gtest/gtest.h>

using namespace exchange::engine;
using namespace exchange;

TEST(Order, EqualOrdersCompareEqual) {
    const Order a{.id = 1, .side = Side::BID, .price = 100, .volume = 10};
    const Order b{.id = 1, .side = Side::BID, .price = 100, .volume = 10};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(Order, OrdersDifferingInAnyFieldAreUnequal) {
    const Order base{.id = 1, .side = Side::BID, .price = 100, .volume = 10};
    EXPECT_NE(base, (Order{.id = 2, .side = Side::BID, .price = 100, .volume = 10}));
    EXPECT_NE(base, (Order{.id = 1, .side = Side::ASK, .price = 100, .volume = 10}));
    EXPECT_NE(base, (Order{.id = 1, .side = Side::BID, .price = 101, .volume = 10}));
    EXPECT_NE(base, (Order{.id = 1, .side = Side::BID, .price = 100, .volume = 11}));
}

TEST(Order, EqualityIgnoresNothingButComparesDefaultedType) {
    // type participates in equality (it is a member); defaults match here.
    const Order a{.id = 7, .side = Side::ASK, .price = 50, .volume = 3};
    const Order b{.id = 7, .side = Side::ASK, .price = 50, .volume = 3,
                  .type = OrderType::FILL_OR_KILL};
    EXPECT_NE(a, b);
}

