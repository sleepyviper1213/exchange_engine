// Formatters for the execution layer: the order manager, its records and the
// handles that name them.

#include "execution/format.hpp"
#include "execution/order_manager.hpp"
#include "orders/types.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

namespace exec = exchange::engine::execution;

using exchange::side_t;
using exchange::engine::OrderStatus;
using exchange::engine::reject_reason;
using exchange::engine::orders::order;

namespace {

TEST(ExecutionFormat, AnEmptyStoreReportsItsCapacity) {
	const exec::order_manager orders{64};
	EXPECT_EQ(fmt::format("{}", orders),
			  "order_manager[live=0 retained=0 peak=0/64]");
}

TEST(ExecutionFormat, AStoreSeparatesLiveRecordsFromRetainedHistory) {
	exec::order_manager orders{64};
	const auto resting = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	const auto done = orders.admit(
		order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(resting.has_value());
	ASSERT_TRUE(done.has_value());
	orders.apply_fill(*done, 10); // terminal, so retained rather than live

	EXPECT_EQ(fmt::format("{}", orders),
			  "order_manager[live=1 retained=1 peak=2/64]");
}

// evicted is the one number here that reports a loss, so it prints only when
// there is one - zero is the healthy case and would be noise on every line.
TEST(ExecutionFormat, AStoreReportsEvictionsOnlyWhenHistoryWasLost) {
	exec::order_manager orders{1};
	const auto first = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(first.has_value());
	orders.apply_fill(*first, 10);
	ASSERT_EQ(fmt::format("{}", orders),
			  "order_manager[live=0 retained=1 peak=1/1]");

	// The only slot there is, taken back - record 1 is gone for good.
	ASSERT_TRUE(
		orders
			.admit(order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10})
			.has_value());
	EXPECT_EQ(fmt::format("{}", orders),
			  "order_manager[live=1 retained=0 peak=1/1 evicted=1]");
}

TEST(ExecutionFormat, ARecordOmitsTheFieldsThatCarryNoInformation) {
	exec::order_manager orders{64};
	const auto handle = orders.admit(order{.id        = 42,
										   .symbol_id = 7,
										   .side      = side_t::ask,
										   .price     = 1250,
										   .qty       = 30});
	ASSERT_TRUE(handle.has_value());

	// Unattributed, unstamped and still live: no acct, no ts, no reason.
	EXPECT_EQ(fmt::format("{}", *orders.get(*handle)),
			  "OrderRecord[id=42 sym=7 ask @1250 0/30 LIVE]");
}

TEST(ExecutionFormat, ARecordShowsProgressAgainstTheOrderQuantity) {
	exec::order_manager orders{64};
	const auto handle = orders.admit(order{.id        = 42,
										   .symbol_id = 7,
										   .side      = side_t::ask,
										   .price     = 1250,
										   .qty       = 30,
										   .timestamp = 1700},
									 99);
	ASSERT_TRUE(handle.has_value());
	orders.apply_fill(*handle, 4);

	EXPECT_EQ(fmt::format("{}", *orders.get(*handle)),
			  "OrderRecord[id=42 sym=7 acct=99 ask @1250 4/30 "
			  "PARTIALLY_FILLED ts=1700]");
}

// A cancel needs no excuse, so its reason stays off the line; an engine-driven
// withdrawal owes one, and that is exactly when it appears.
TEST(ExecutionFormat, ARecordPrintsAReasonOnlyWhenTheOrderEndedWithOne) {
	exec::order_manager orders{64};
	const auto plain = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	const auto caused = orders.admit(
		order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(plain.has_value());
	ASSERT_TRUE(caused.has_value());

	orders.cancel(*plain);
	orders.cancel(*caused, reject_reason::TIME_IN_FORCE);

	EXPECT_EQ(fmt::format("{}", *orders.get(*plain)),
			  "OrderRecord[id=1 sym=0 bid @100 0/10 CANCELLED]");
	EXPECT_EQ(fmt::format("{}", *orders.get(*caused)),
			  "OrderRecord[id=2 sym=0 bid @100 0/10 CANCELLED TIME_IN_FORCE]");
}

// Two handles naming the same slot at different generations are different
// orders; a rendering that showed only the slot would hide exactly that.
TEST(ExecutionFormat, AHandlePrintsItsGenerationAndSaysWhenItIsNull) {
	exec::order_manager orders{1};
	const auto first = orders.admit(
		order{.id = 1, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(first.has_value());
	orders.cancel(*first);
	EXPECT_EQ(fmt::format("{}", *first), "order_handle[slot=0 gen=0]");

	const auto second = orders.admit(
		order{.id = 2, .side = side_t::bid, .price = 100, .qty = 10});
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(fmt::format("{}", *second), "order_handle[slot=0 gen=1]");

	EXPECT_EQ(fmt::format("{}", exec::order_handle{}), "order_handle[none]");
}

// Every formatter here derives from nested_formatter, so fill/align/width apply
// to the whole record - the property that makes them usable in a log column.
TEST(ExecutionFormat, StoreRenderingsHonourFillAlignAndWidth) {
	const exec::order_manager orders{64};
	EXPECT_EQ(fmt::format("{:>45}", orders),
			  "   order_manager[live=0 retained=0 peak=0/64]");
	EXPECT_EQ(fmt::format("{:.<28}", exec::order_handle{}),
			  "order_handle[none]..........");
}

} // namespace
