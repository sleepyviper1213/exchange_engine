#include "order_book/order_state.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace exchange;
using exchange::engine::OrderStatus;
using exchange::engine::order_state;
using testing::HasSubstr;

TEST(OrderStateDeathTest, OverfillIsRejected) {
	order_state state{10 * units::lot};
	state.apply_fill(6 * units::lot);
	EXPECT_DEBUG_DEATH(state.apply_fill(5 * units::lot),
					   HasSubstr("overfill: fill exceeds remaining quantity"));
}

TEST(OrderStateDeathTest, FillOnATerminalOrderIsRejected) {
	order_state state{10 * units::lot};
	state.apply_fill(10 * units::lot); // FILLED
	EXPECT_DEBUG_DEATH(state.apply_fill(1 * units::lot),
					   HasSubstr("fill on a terminal order"));
}

TEST(OrderStateDeathTest, FillOnACancelledOrderIsRejected) {
	order_state state{10 * units::lot};
	state.cancel();
	// Same precondition as the filled case: cancelled is terminal too, and the
	// fill is rejected for being a fill, not for the route that got it there.
	EXPECT_DEBUG_DEATH(state.apply_fill(1 * units::lot),
					   HasSubstr("fill on a terminal order"));
}

TEST(OrderStateDeathTest, DoubleCancelIsRejected) {
	order_state state{10 * units::lot};
	state.cancel();
	EXPECT_DEBUG_DEATH(state.cancel(), HasSubstr("cancel on a terminal order"));
}

TEST(OrderStateDeathTest, ModifyAtOrBelowExecutedQuantityIsRejected) {
	order_state state{10 * units::lot};
	state.apply_fill(4 * units::lot);
	EXPECT_DEBUG_DEATH(state.modify(4 * units::lot),
					   HasSubstr("modify below executed quantity"));
}

TEST(OrderStateDeathTest, NonPositiveQuantityIsRejected) {
	EXPECT_DEBUG_DEATH(order_state{0 * units::lot},
					   HasSubstr("order quantity must be positive"));
}
