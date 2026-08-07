#include "order_manager.fixture.hpp"
#include "trading-engine/execution/order_manager.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// The preconditions, kept in their own binary-forking file. Each one is a
// statement the caller guarantees, not input the manager validates: a handle
// that names no live record has already been recycled or retired, and acting on
// it would mean the engine and the venue disagree about which orders exist.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::execution;
using order_manager_test::limit;
using testing::HasSubstr;

TEST(OrderManagerDeathTest, FillingARetiredOrderIsRejected) {
	order_manager manager{8};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.apply_fill(*handle, 10); // FILLED, and retired with it

	EXPECT_DEBUG_DEATH(manager.apply_fill(*handle, 1),
					   HasSubstr("apply_fill(): handle names no live order"));
}

TEST(OrderManagerDeathTest, FillingThroughAStaleHandleIsRejected) {
	order_manager manager{1};
	const auto first = manager.admit(limit(1, 10));
	ASSERT_TRUE(first.has_value());
	manager.cancel(*first);
	ASSERT_TRUE(manager.admit(limit(2, 10)).has_value()); // recycles the slot

	// The generation check turns this from a fill against someone else's order
	// into a refusal.
	EXPECT_DEBUG_DEATH(manager.apply_fill(*first, 1),
					   HasSubstr("apply_fill(): handle names no live order"));
}

TEST(OrderManagerDeathTest, CancellingTwiceIsRejected) {
	order_manager manager{8};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.cancel(*handle);

	EXPECT_DEBUG_DEATH(manager.cancel(*handle),
					   HasSubstr("cancel(): handle names no live order"));
}

TEST(OrderManagerDeathTest, CancellingANullHandleIsRejected) {
	order_manager manager{8};
	EXPECT_DEBUG_DEATH(manager.cancel(order_handle{}),
					   HasSubstr("cancel(): handle names no live order"));
}

// An order that executed entered the book by definition, so "it never entered
// the book" is not a fact anyone may record about it — that is a cancel.
TEST(OrderManagerDeathTest, RejectingAnOrderThatAlreadyTradedIsRejected) {
	order_manager manager{8};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.apply_fill(*handle, 4);

	EXPECT_DEBUG_DEATH(
		manager.reject(*handle, reject_reason::INSUFFICIENT_LIQUIDITY),
		HasSubstr("an order that executed entered the book"));
}

TEST(OrderManagerDeathTest, RejectingATerminalOrderIsRejected) {
	order_manager manager{8};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.cancel(*handle);

	EXPECT_DEBUG_DEATH(
		manager.reject(*handle, reject_reason::INSUFFICIENT_LIQUIDITY),
		HasSubstr("reject(): handle names no live order"));
}
