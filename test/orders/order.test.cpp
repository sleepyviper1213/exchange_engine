#include "orders/order.hpp"

#include <gtest/gtest.h>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;

TEST(order, EqualOrdersCompareEqual) {
    const order a{.id = 1, .side = side_t::bid, .price = 100, .qty = 10};
    const order b{.id = 1, .side = side_t::bid, .price = 100, .qty = 10};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(order, OrdersDifferingInAnyFieldAreUnequal) {
    const order base{.id = 1, .side = side_t::bid, .price = 100, .qty = 10};
    EXPECT_NE(base, (order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10}));
    EXPECT_NE(base, (order{.id = 1, .side = side_t::ask, .price = 100, .qty = 10}));
    EXPECT_NE(base, (order{.id = 1, .side = side_t::bid, .price = 101, .qty = 10}));
    EXPECT_NE(base, (order{.id = 1, .side = side_t::bid, .price = 100, .qty = 11}));
}

TEST(order, EqualityIgnoresNothingButComparesDefaultedType) {
    // type participates in equality (it is a member); defaults match here.
    const order a{.id = 7, .side = side_t::ask, .price = 50, .qty = 3};
    const order b{.id = 7, .side = side_t::ask,
                  .tif = time_in_force_instruction::FILL_OR_KILL,
                  .price = 50, .qty = 3};
    EXPECT_NE(a, b);
}

