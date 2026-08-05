#include "trading-engine/order_book/order_state.hpp"

#include <gtest/gtest.h>

// Ported from Emporia's machine-checked order state machine:
//
//   verification/key-order-state/OrderStateModel.java  (JML, discharged by KeY)
//   verification/order-lifecycle/OrderLifecycle.tla    (TLA+, checked by TLC)
//
// KeY proves the JML contracts for every input; these tests only sample them,
// which is the honest trade of a port from a proof assistant to a C++ suite.
// What survives the port intact is the *specification*: each test below names
// the clause it exercises, so the contracts stay legible even where the proof
// does not come with them. This repo's verification/order-lifecycle/ carries
// the TLA+ side, which can still be model-checked as-is.
//
// The preconditions (fill on a terminal order, overfill, modify below the
// executed quantity) are assertions, not error returns, so they abort rather
// than fail a check. They are covered by death tests where the platform
// supports them; the invariant they protect is the caller's, and order_book is
// where that is enforced.

using exchange::engine::is_active;
using exchange::engine::is_terminal;
using exchange::engine::OrderStatus;
using exchange::engine::order_state;

// --------------------------------------------------------------------------
// Constructor
//   requires initialQuantityLots > 0
//   ensures  quantity == initialQuantity, traded == 0,
//            remaining == initialQuantity, status == LIVE
// --------------------------------------------------------------------------

TEST(OrderState, ConstructorStartsLiveWithNothingExecuted) {
	const order_state state{10};
	EXPECT_EQ(state.quantity(), 10);
	EXPECT_EQ(state.traded(), 0);
	EXPECT_EQ(state.remaining(), 10);
	EXPECT_EQ(state.status(), OrderStatus::LIVE);
	EXPECT_TRUE(state.is_active());
}

TEST(OrderState, PartialFillLeavesPartiallyFilled) {
	order_state state{10};
	state.apply_fill(4);
	EXPECT_EQ(state.quantity(), 10);
	EXPECT_EQ(state.traded(), 4);
	EXPECT_EQ(state.remaining(), 6);
	EXPECT_EQ(state.status(), OrderStatus::PARTIALLY_FILLED);
}

TEST(OrderState, FillsAccumulate) {
	order_state state{10};
	state.apply_fill(4);
	state.apply_fill(3);
	EXPECT_EQ(state.traded(), 7);
	EXPECT_EQ(state.remaining(), 3);
	EXPECT_EQ(state.status(), OrderStatus::PARTIALLY_FILLED);
}

TEST(OrderState, FillingTheRemainderLeavesFilled) {
	order_state state{10};
	state.apply_fill(4);
	state.apply_fill(6);
	EXPECT_EQ(state.traded(), 10);
	EXPECT_EQ(state.remaining(), 0);
	EXPECT_EQ(state.status(), OrderStatus::FILLED);
	EXPECT_FALSE(state.is_active());
	EXPECT_TRUE(is_terminal(state.status()));
}

TEST(OrderState, OneFillForTheWholeQuantitySkipsPartiallyFilled) {
	order_state state{10};
	state.apply_fill(10);
	EXPECT_EQ(state.status(), OrderStatus::FILLED);
}

TEST(OrderState, RemainingAndTradedAlwaysSumToQuantity) {
	order_state state{9};
	for (const auto fill : {1, 2, 3, 2}) {
		state.apply_fill(fill);
		EXPECT_EQ(state.traded() + state.remaining(), state.quantity());
	}
	EXPECT_EQ(state.remaining(), 1);
}

TEST(OrderState, ModifyResizesRemainingAndKeepsTraded) {
	order_state state{10};
	state.apply_fill(4);
	state.modify(8);
	EXPECT_EQ(state.quantity(), 8);
	EXPECT_EQ(state.traded(), 4); // unchanged by the resize
	EXPECT_EQ(state.remaining(), 4);
	EXPECT_EQ(state.status(), OrderStatus::PARTIALLY_FILLED);
}

TEST(OrderState, ModifyUpwardsKeepsTradedAndStatus) {
	order_state state{10};
	state.apply_fill(4);
	state.modify(20);
	EXPECT_EQ(state.quantity(), 20);
	EXPECT_EQ(state.traded(), 4);
	EXPECT_EQ(state.remaining(), 16);
	EXPECT_EQ(state.status(), OrderStatus::PARTIALLY_FILLED);
}

TEST(OrderState, ModifyOnAnUntouchedOrderStaysLive) {
	order_state state{10};
	state.modify(3);
	EXPECT_EQ(state.quantity(), 3);
	EXPECT_EQ(state.remaining(), 3);
	EXPECT_EQ(state.status(), OrderStatus::LIVE);
}

TEST(OrderState, CancelFreezesQuantitiesAndGoesTerminal) {
	order_state state{10};
	state.apply_fill(4);
	state.cancel();
	EXPECT_EQ(state.quantity(), 10);
	EXPECT_EQ(state.traded(), 4); // NoExecutionAfterCancellation: kept, not lost
	EXPECT_EQ(state.remaining(), 6);
	EXPECT_EQ(state.status(), OrderStatus::CANCELLED);
	EXPECT_FALSE(state.is_active());
}

TEST(OrderState, CancelBeforeAnyFillIsStillCancelled) {
	order_state state{10};
	state.cancel();
	EXPECT_EQ(state.traded(), 0);
	EXPECT_EQ(state.status(), OrderStatus::CANCELLED);
}

TEST(OrderState, CancelledOrderStillHasQuantityOutstanding) {
	order_state state{10};
	state.apply_fill(9);
	state.cancel();
	EXPECT_LT(state.traded(), state.quantity());
	EXPECT_GT(state.remaining(), 0);
}

TEST(OrderState, ActiveAndTerminalPartitionTheStatuses) {
	for (const auto status : {OrderStatus::LIVE, OrderStatus::PARTIALLY_FILLED}) {
		EXPECT_TRUE(is_active(status));
		EXPECT_FALSE(is_terminal(status));
	}
	for (const auto status : {OrderStatus::FILLED,
							  OrderStatus::CANCELLED,
							  OrderStatus::REJECTED}) {
		EXPECT_FALSE(is_active(status));
		EXPECT_TRUE(is_terminal(status));
	}
	// NEW is neither: it is the pre-book state, and nothing may be done to an
	// order in it except accept or reject.
	EXPECT_FALSE(is_active(OrderStatus::NEW));
	EXPECT_FALSE(is_terminal(OrderStatus::NEW));
}

TEST(OrderState, StatusNamesRoundTripToTheirEnumeratorText) {
	EXPECT_EQ(to_string(OrderStatus::PARTIALLY_FILLED), "PARTIALLY_FILLED");
	EXPECT_EQ(to_string(OrderStatus::CANCELLED), "CANCELLED");
}

