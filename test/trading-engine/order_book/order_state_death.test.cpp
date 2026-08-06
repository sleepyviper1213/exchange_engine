#include "trading-engine/order_book/order_state.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using exchange::engine::OrderStatus;
using exchange::engine::order_state;
using testing::HasSubstr;

TEST(OrderStateDeathTest, OverfillIsRejected) {
	order_state state{10};
	state.apply_fill(6);
	EXPECT_DEBUG_DEATH(state.apply_fill(5),
					   HasSubstr("overfill: fill exceeds remaining quantity"));
}

TEST(OrderStateDeathTest, FillOnATerminalOrderIsRejected) {
	order_state state{10};
	state.apply_fill(10); // FILLED
	EXPECT_DEBUG_DEATH(state.apply_fill(1),
					   HasSubstr("fill on a terminal order"));
}

TEST(OrderStateDeathTest, FillOnACancelledOrderIsRejected) {
	order_state state{10};
	state.cancel();
	// Same precondition as the filled case: cancelled is terminal too, and the
	// fill is rejected for being a fill, not for the route that got it there.
	EXPECT_DEBUG_DEATH(state.apply_fill(1),
					   HasSubstr("fill on a terminal order"));
}

TEST(OrderStateDeathTest, DoubleCancelIsRejected) {
	order_state state{10};
	state.cancel();
	EXPECT_DEBUG_DEATH(state.cancel(), HasSubstr("cancel on a terminal order"));
}

TEST(OrderStateDeathTest, ModifyAtOrBelowExecutedQuantityIsRejected) {
	order_state state{10};
	state.apply_fill(4);
	EXPECT_DEBUG_DEATH(state.modify(4),
					   HasSubstr("modify below executed quantity"));
}

TEST(OrderStateDeathTest, NonPositiveQuantityIsRejected) {
	EXPECT_DEBUG_DEATH(order_state{0},
					   HasSubstr("order quantity must be positive"));
}
