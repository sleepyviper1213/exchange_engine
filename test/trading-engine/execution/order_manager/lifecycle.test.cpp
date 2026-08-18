#include "order_manager.fixture.hpp"
#include "trading-engine/execution/order_manager.hpp"

#include <gtest/gtest.h>

// What a record says after each transition, and - the part the book cannot do -
// that it keeps saying it once the order is terminal. A retired record is still
// a record: it leaves the live population and stays resolvable.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::execution;
using order_manager_test::limit;

TEST(OrderManagerLifecycle, APartialFillLeavesTheOrderLiveAndCountsWhatTraded) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());

	manager.apply_fill(*handle, 4);

	const order_record *record = manager.get(*handle);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->status(), OrderStatus::PARTIALLY_FILLED);
	EXPECT_EQ(record->state.traded(), 4);
	EXPECT_EQ(record->state.remaining(), 6);
	EXPECT_TRUE(record->is_active());

	EXPECT_EQ(manager.live(), 1u);
	EXPECT_EQ(manager.retained(), 0u);
}

// Filling to zero is the moment the order stops being live and starts being
// history, and both halves happen in the same step - is_alive() counts orders the
// book could still act on, and this is no longer one of them.
TEST(OrderManagerLifecycle, AFillThatCompletesTheOrderRetiresItButKeepsIt) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());

	manager.apply_fill(*handle, 6);
	manager.apply_fill(*handle, 4);

	const order_record *record = manager.get(*handle);
	ASSERT_NE(record, nullptr) << "a retired record is still resolvable";
	EXPECT_EQ(record->status(), OrderStatus::FILLED);
	EXPECT_EQ(record->state.traded(), 10);
	EXPECT_EQ(record->state.remaining(), 0);
	EXPECT_FALSE(record->is_active());

	EXPECT_EQ(manager.live(), 0u);
	EXPECT_EQ(manager.retained(), 1u);
	EXPECT_EQ(manager.size(), 1u);
	EXPECT_TRUE(manager.contains(1));
}

// A cancellation withdraws the remainder; it does not undo the fills. The
// executed quantity is what a client is owed a report on and what the record
// must keep.
TEST(OrderManagerLifecycle, CancelFreezesTheExecutedQuantity) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.apply_fill(*handle, 4);

	manager.cancel(*handle);

	const order_record *record = manager.get(*handle);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->status(), OrderStatus::CANCELLED);
	EXPECT_EQ(record->state.traded(), 4);
	EXPECT_EQ(record->state.remaining(), 6);
	// A client cancel needs no excuse, so the reason stays NONE.
	EXPECT_EQ(record->reason, reject_reason::NONE);
	EXPECT_EQ(manager.live(), 0u);
	EXPECT_EQ(manager.retained(), 1u);
}

// The engine withdrawing a remainder on the client's behalf is still a cancel,
// but it owes a cause - an IOC remainder went because of the instruction, not
// because anyone asked.
TEST(OrderManagerLifecycle, CancelCarriesTheCauseWhenTheEngineWithdrewIt) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());

	manager.cancel(*handle, reject_reason::TIME_IN_FORCE);

	const order_record *record = manager.get(*handle);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->status(), OrderStatus::CANCELLED);
	EXPECT_EQ(record->reason, reject_reason::TIME_IN_FORCE);
}

// REJECTED is the one status the quantities cannot express: nothing traded and
// the remainder withdrawn is exactly what a never-filled cancel looks like. The
// record carries the extra bit so a client can tell "never happened" from "was
// live and I withdrew it".
TEST(OrderManagerLifecycle, RejectIsDistinguishableFromACancelThatNeverFilled) {
	order_manager manager{64};
	const auto rejected = manager.admit(limit(1, 10));
	const auto cancelled = manager.admit(limit(2, 10));
	ASSERT_TRUE(rejected.has_value());
	ASSERT_TRUE(cancelled.has_value());

	manager.reject(*rejected, reject_reason::INSUFFICIENT_LIQUIDITY);
	manager.cancel(*cancelled);

	const order_record *rejected_record = manager.get(*rejected);
	const order_record *cancelled_record = manager.get(*cancelled);
	ASSERT_NE(rejected_record, nullptr);
	ASSERT_NE(cancelled_record, nullptr);

	// Identical quantities...
	EXPECT_EQ(rejected_record->state.traded(), 0);
	EXPECT_EQ(cancelled_record->state.traded(), 0);
	EXPECT_EQ(rejected_record->state.remaining(), 10);
	EXPECT_EQ(cancelled_record->state.remaining(), 10);
	// ...different facts about the order.
	EXPECT_EQ(rejected_record->status(), OrderStatus::REJECTED);
	EXPECT_EQ(cancelled_record->status(), OrderStatus::CANCELLED);
	EXPECT_EQ(rejected_record->reason, reject_reason::INSUFFICIENT_LIQUIDITY);
	EXPECT_FALSE(rejected_record->is_active());

	EXPECT_EQ(manager.live(), 0u);
	EXPECT_EQ(manager.retained(), 2u);
}

// find() is the by-id route to the same record, and it must agree with the
// handle the admission returned - otherwise a client looking its own order up
// and the engine acting on it would be talking about different rows.
TEST(OrderManagerLifecycle, FindByIdAgreesWithTheAdmittedHandle) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(5, 10));
	ASSERT_TRUE(handle.has_value());

	EXPECT_EQ(manager.find(5), *handle);
	EXPECT_EQ(manager.find_record(5), manager.get(*handle));

	EXPECT_FALSE(manager.find(6).valid());
	EXPECT_EQ(manager.find_record(6), nullptr);
}

// A handle nobody was issued must not resolve, or a caller could reach a record
// by guessing a slot index.
TEST(OrderManagerLifecycle, ANullOrUnissuedHandleResolvesToNothing) {
	order_manager manager{64};
	ASSERT_TRUE(manager.admit(limit(1)).has_value());

	EXPECT_EQ(manager.get(order_handle{}), nullptr);
	// Slot 1 exists but has never been handed out; slot 999 is past the table.
	EXPECT_EQ(manager.get(order_handle{.slot = 1, .generation = 0}), nullptr);
	EXPECT_EQ(manager.get(order_handle{.slot = 999, .generation = 0}), nullptr);
}
