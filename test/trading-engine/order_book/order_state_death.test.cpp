#include "trading-engine/order_book/order_state.hpp"

#include <gtest/gtest.h>

using exchange::engine::OrderStatus;
using exchange::engine::order_state;

// Death tests for order_state preconditions. Separated from order_state.test.cpp
// because gtest runs a death test by re-executing the binary, and keeping them
// in their own translation unit keeps that cost visible.

TEST(OrderStateDeathTest, OverfillIsRejected) {
	order_state state{10};
	state.apply_fill(6);
	EXPECT_DEATH(state.apply_fill(5), "overfill");
}

TEST(OrderStateDeathTest, FillOnATerminalOrderIsRejected) {
	order_state state{10};
	state.apply_fill(10); // FILLED
	EXPECT_DEATH(state.apply_fill(1), "terminal");
}

TEST(OrderStateDeathTest, FillOnACancelledOrderIsRejected) {
	order_state state{10};
	state.cancel();
	EXPECT_DEATH(state.apply_fill(1), "terminal");
}

TEST(OrderStateDeathTest, DoubleCancelIsRejected) {
	order_state state{10};
	state.cancel();
	EXPECT_DEATH(state.cancel(), "terminal");
}

TEST(OrderStateDeathTest, ModifyAtOrBelowExecutedQuantityIsRejected) {
	order_state state{10};
	state.apply_fill(4);
	EXPECT_DEATH(state.modify(4), "executed quantity");
}

TEST(OrderStateDeathTest, NonPositiveQuantityIsRejected) {
	EXPECT_DEATH(order_state{0}, "positive");
}

