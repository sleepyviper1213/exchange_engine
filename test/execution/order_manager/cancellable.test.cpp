#include "order_manager.fixture.hpp"
#include "execution/order_manager.hpp"

#include <gtest/gtest.h>

// The question the whole component exists to answer. order_book::cancel_order
// probes an index holding only *resting* orders, so "filled a microsecond ago",
// "already cancelled" and "never placed" all come back as one empty probe and
// one UNKNOWN_ORDER. Here they are three different records and three different
// answers - and where the manager genuinely cannot tell, it still says
// UNKNOWN_ORDER rather than guessing.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::execution;
using order_manager_test::limit;

TEST(OrderManagerCancellable, ALiveOrderCanBeCancelled) {
	order_manager manager{64};
	ASSERT_TRUE(manager.admit(limit(1, 10)).has_value());
	EXPECT_EQ(manager.cancellable(1), reject_reason::NONE);
}

TEST(OrderManagerCancellable, APartiallyFilledOrderCanStillBeCancelled) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.apply_fill(*handle, 4);

	EXPECT_EQ(manager.cancellable(1), reject_reason::NONE);
}

// The cancel/fill race, told honestly. The client sent a cancel; the order had
// already filled. Under the book alone this is UNKNOWN_ORDER and the client is
// left wondering whether the order ever existed.
TEST(OrderManagerCancellable, AFilledOrderSaysSoRatherThanUnknown) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.apply_fill(*handle, 10);

	EXPECT_EQ(manager.cancellable(1), reject_reason::ORDER_ALREADY_FILLED);
}

TEST(OrderManagerCancellable, ACancelledOrderSaysSoRatherThanUnknown) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.cancel(*handle);

	EXPECT_EQ(manager.cancellable(1), reject_reason::ORDER_ALREADY_CANCELLED);
}

TEST(OrderManagerCancellable, ARejectedOrderSaysSoRatherThanUnknown) {
	order_manager manager{64};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.reject(*handle, reject_reason::INSUFFICIENT_LIQUIDITY);

	EXPECT_EQ(manager.cancellable(1), reject_reason::ORDER_ALREADY_REJECTED);
}

TEST(OrderManagerCancellable, AnIdNobodyEverPlacedIsUnknown) {
	order_manager manager{64};
	EXPECT_EQ(manager.cancellable(1), reject_reason::UNKNOWN_ORDER);
}

// The honest limit of a bounded history: once a record has aged out, "never
// placed" and "placed and long since finished" really are indistinguishable, and
// the manager says the same thing the book would have.
TEST(OrderManagerCancellable, AnOrderEvictedFromHistoryIsUnknownAgain) {
	order_manager manager{1};
	const auto handle = manager.admit(limit(1, 10));
	ASSERT_TRUE(handle.has_value());
	manager.apply_fill(*handle, 10);
	ASSERT_EQ(manager.cancellable(1), reject_reason::ORDER_ALREADY_FILLED);

	// The only slot there is, taken back by the next order.
	ASSERT_TRUE(manager.admit(limit(2, 10)).has_value());

	EXPECT_EQ(manager.cancellable(1), reject_reason::UNKNOWN_ORDER);
	EXPECT_EQ(manager.cancellable(2), reject_reason::NONE);
}
