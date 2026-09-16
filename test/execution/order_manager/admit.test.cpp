#include "order_manager.fixture.hpp"
#include "execution/order_manager.hpp"

#include <gtest/gtest.h>

// The admission boundary. What matters here is not that a record appears - it is
// which orders are refused and with which reason, because those refusals are the
// ones the book cannot make on its own.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::execution;
using order_manager_test::limit;

TEST(OrderManagerAdmit, AnEmptyManagerHoldsNothingAndKnowsItsCapacity) {
	const order_manager manager{64};
	EXPECT_EQ(manager.capacity(), 64u);
	EXPECT_EQ(manager.live(), 0u);
	EXPECT_EQ(manager.retained(), 0u);
	EXPECT_EQ(manager.size(), 0u);
	EXPECT_EQ(manager.high_water(), 0u);
	EXPECT_EQ(manager.evicted(), 0u);
	EXPECT_FALSE(manager.contains(1));
}

TEST(OrderManagerAdmit, AdmittedOrderIsLiveAndCarriesEveryFieldItArrivedWith) {
	order_manager manager{64};
	const orders::order incoming{
		.id        = 42,
		.symbol_id = 7,
		.side      = side_t::ask,
		.type      = orders::order_type::LIMIT,
		.tif       = orders::time_in_force_instruction::IMMEDIATE_OR_CANCEL,
		.price     = 1'250,
		.qty       = 30,
		.timestamp = 1'700'000'000'000'000'000ULL};

	const auto admitted = manager.admit(incoming, 99);
	ASSERT_TRUE(admitted.has_value());

	const order_record *record = manager.get(*admitted);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->id, 42u);
	EXPECT_EQ(record->symbol, 7u);
	EXPECT_EQ(record->account, 99u);
	EXPECT_EQ(record->price, 1'250u);
	EXPECT_EQ(record->side, side_t::ask);
	EXPECT_EQ(record->type, orders::order_type::LIMIT);
	EXPECT_EQ(record->tif,
			  orders::time_in_force_instruction::IMMEDIATE_OR_CANCEL);
	EXPECT_EQ(record->timestamp, 1'700'000'000'000'000'000ULL);
	EXPECT_EQ(record->state.quantity(), 30);
	EXPECT_EQ(record->state.traded(), 0);
	EXPECT_EQ(record->state.remaining(), 30);
	EXPECT_EQ(status(*record), OrderStatus::LIVE);
	EXPECT_EQ(record->reason, reject_reason::NONE);
	EXPECT_TRUE(is_active(*record));

	EXPECT_EQ(manager.live(), 1u);
	EXPECT_EQ(manager.size(), 1u);
	EXPECT_EQ(manager.high_water(), 1u);
	EXPECT_TRUE(manager.contains(42));
}

// Id 0 is the book's anonymous sentinel and this table's vacant-slot marker.
// Admitting one would make a live record indistinguishable from an empty slot.
TEST(OrderManagerAdmit, TheAnonymousIdIsRefused) {
	order_manager manager{64};
	const auto admitted = manager.admit(limit(0));
	ASSERT_FALSE(admitted.has_value());
	EXPECT_EQ(admitted.error(), reject_reason::RESERVED_ORDER_ID);
	EXPECT_EQ(manager.size(), 0u);
}

// The same boundary the book enforces: there is no representable order_state for
// a non-positive order, so this is what keeps the invariant true rather than
// merely asserted.
TEST(OrderManagerAdmit, NonPositiveQuantityIsRefused) {
	order_manager manager{64};

	const auto zero = manager.admit(limit(1, 0));
	ASSERT_FALSE(zero.has_value());
	EXPECT_EQ(zero.error(), reject_reason::NON_POSITIVE_QUANTITY);

	const auto negative = manager.admit(limit(2, -5));
	ASSERT_FALSE(negative.has_value());
	EXPECT_EQ(negative.error(), reject_reason::NON_POSITIVE_QUANTITY);

	EXPECT_EQ(manager.size(), 0u);
}

TEST(OrderManagerAdmit, AnIdAlreadyLiveIsRefused) {
	order_manager manager{64};
	ASSERT_TRUE(manager.admit(limit(7)).has_value());

	const auto second = manager.admit(limit(7));
	ASSERT_FALSE(second.has_value());
	EXPECT_EQ(second.error(), reject_reason::DUPLICATE_ORDER_ID);
	EXPECT_EQ(manager.live(), 1u);
}

// The headline difference from the book. order_book forgets an order the instant
// it fills, so it would take the id again and hand one client two lifecycles
// under one name; here the record outlives the order and the id stays spent.
TEST(OrderManagerAdmit, AnIdIsStillSpentAfterTheOrderHasFilled) {
	order_manager manager{64};
	const auto first = manager.admit(limit(7, 10));
	ASSERT_TRUE(first.has_value());
	manager.apply_fill(*first, 10);
	ASSERT_EQ(manager.live(), 0u);
	ASSERT_EQ(manager.retained(), 1u);

	const auto reused = manager.admit(limit(7, 10));
	ASSERT_FALSE(reused.has_value());
	EXPECT_EQ(reused.error(), reject_reason::DUPLICATE_ORDER_ID);
}

TEST(OrderManagerAdmit, AnIdIsStillSpentAfterTheOrderWasCancelled) {
	order_manager manager{64};
	const auto first = manager.admit(limit(7));
	ASSERT_TRUE(first.has_value());
	manager.cancel(*first);

	const auto reused = manager.admit(limit(7));
	ASSERT_FALSE(reused.has_value());
	EXPECT_EQ(reused.error(), reject_reason::DUPLICATE_ORDER_ID);
}

// A live record is never recycled, so a table full of live orders has to refuse
// rather than evict - the alternative is the book holding an order the venue has
// no record of.
TEST(OrderManagerAdmit, AFullTableOfLiveOrdersRefusesRatherThanEvicts) {
	order_manager manager{2};
	ASSERT_TRUE(manager.admit(limit(1)).has_value());
	ASSERT_TRUE(manager.admit(limit(2)).has_value());

	const auto third = manager.admit(limit(3));
	ASSERT_FALSE(third.has_value());
	EXPECT_EQ(third.error(), reject_reason::BOOK_AT_CAPACITY);

	// Neither live order was disturbed, and neither was silently dropped.
	EXPECT_TRUE(manager.contains(1));
	EXPECT_TRUE(manager.contains(2));
	EXPECT_EQ(manager.live(), 2u);
	EXPECT_EQ(manager.evicted(), 0u);
}

// The capacity-planning reading: it tracks the peak, not the current count, and
// survives the orders leaving.
TEST(OrderManagerAdmit, HighWaterTracksThePeakAndSurvivesTheOrdersLeaving) {
	order_manager manager{64};
	const auto first  = manager.admit(limit(1));
	const auto second = manager.admit(limit(2));
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(manager.high_water(), 2u);

	manager.cancel(*first);
	manager.cancel(*second);

	EXPECT_EQ(manager.live(), 0u);
	EXPECT_EQ(manager.high_water(), 2u);
}
