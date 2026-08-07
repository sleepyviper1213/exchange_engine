#include "order_manager.fixture.hpp"
#include "trading-engine/execution/order_manager.hpp"

#include <gtest/gtest.h>

#include <cstdint>

// The retention policy, which is the only part of this component that can be
// wrong in a way nothing else notices. Slots are finite; history is kept for as
// long as there is room and given up oldest-first, and a handle that outlives
// its record has to read as stale rather than as its replacement.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::execution;
using order_manager_test::limit;

// Fresh slots first: a manager with room left never touches history, however
// many orders have already come and gone.
TEST(OrderManagerRecycling, UntouchedSlotsAreSpentBeforeAnyHistoryIsGivenUp) {
	order_manager manager{4};
	const auto first = manager.admit(limit(1));
	ASSERT_TRUE(first.has_value());
	manager.cancel(*first); // retired immediately, but not yet needed

	ASSERT_TRUE(manager.admit(limit(2)).has_value());
	ASSERT_TRUE(manager.admit(limit(3)).has_value());

	EXPECT_TRUE(manager.contains(1)) << "history given up with slots to spare";
	EXPECT_EQ(manager.evicted(), 0u);
	EXPECT_EQ(manager.size(), 3u);
}

// Oldest terminal record first — the one that stopped mattering longest ago.
TEST(OrderManagerRecycling, TheOldestTerminalRecordIsTheOneEvicted) {
	order_manager manager{3};
	const auto first  = manager.admit(limit(1));
	const auto second = manager.admit(limit(2));
	const auto third  = manager.admit(limit(3));
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_TRUE(third.has_value());

	manager.cancel(*second); // retired first
	manager.cancel(*first);  // retired second
	manager.cancel(*third);  // retired third
	ASSERT_EQ(manager.retained(), 3u);

	ASSERT_TRUE(manager.admit(limit(4)).has_value());

	EXPECT_FALSE(manager.contains(2)) << "retired earliest, so evicted first";
	EXPECT_TRUE(manager.contains(1));
	EXPECT_TRUE(manager.contains(3));
	EXPECT_EQ(manager.evicted(), 1u);
	EXPECT_EQ(manager.live(), 1u);
	EXPECT_EQ(manager.retained(), 2u);
}

// The generation counter earning its keep. Without it, a handle held across a
// recycle would silently name whichever client's order landed in the slot next —
// a wrong answer, which is worse than no answer.
TEST(OrderManagerRecycling, AHandleWhoseSlotWasRecycledGoesStale) {
	order_manager manager{1};
	const auto first = manager.admit(limit(1, 10));
	ASSERT_TRUE(first.has_value());
	manager.cancel(*first);
	ASSERT_NE(manager.get(*first), nullptr) << "retired, not yet recycled";

	const auto second = manager.admit(limit(2, 20));
	ASSERT_TRUE(second.has_value());

	EXPECT_EQ(manager.get(*first), nullptr) << "same slot, later order";
	EXPECT_EQ(first->slot, second->slot);
	EXPECT_NE(first->generation, second->generation);

	const order_record *record = manager.get(*second);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->id, 2u);
	EXPECT_EQ(record->state.quantity(), 20);
}

// An id freed by eviction is available again — it has to be, or a long-running
// venue would eventually refuse every id a client owns.
TEST(OrderManagerRecycling, AnEvictedIdCanBeAdmittedAgain) {
	order_manager manager{1};
	const auto first = manager.admit(limit(1, 10));
	ASSERT_TRUE(first.has_value());
	manager.apply_fill(*first, 10);

	const auto second = manager.admit(limit(2, 10)); // evicts record 1
	ASSERT_TRUE(second.has_value());
	ASSERT_FALSE(manager.contains(1));
	manager.apply_fill(*second, 10);

	const auto reused = manager.admit(limit(1, 10)); // evicts record 2
	ASSERT_TRUE(reused.has_value()) << "the id is spent only while remembered";
	EXPECT_FALSE(manager.contains(2));
	EXPECT_TRUE(manager.contains(1));
}

// Steady state: a venue that churns far more orders than it has slots must keep
// working, keep its counters straight, and never grow.
TEST(OrderManagerRecycling, SustainedChurnPastCapacityKeepsTheCountersHonest) {
	constexpr std::uint32_t CAPACITY = 8;
	constexpr order_id_t ORDERS      = 500;
	order_manager manager{CAPACITY};

	for (order_id_t id = 1; id <= ORDERS; ++id) {
		const auto handle = manager.admit(limit(id, 10));
		ASSERT_TRUE(handle.has_value()) << "refused order " << id;
		manager.apply_fill(*handle, 10);
	}

	EXPECT_EQ(manager.live(), 0u);
	EXPECT_EQ(manager.retained(), CAPACITY);
	EXPECT_EQ(manager.size(), CAPACITY);
	EXPECT_EQ(manager.capacity(), CAPACITY);
	EXPECT_EQ(manager.high_water(), 1u) << "one order live at a time";
	EXPECT_EQ(manager.evicted(), ORDERS - CAPACITY);

	// The survivors are the last CAPACITY orders, in order.
	for (order_id_t id = ORDERS - CAPACITY + 1; id <= ORDERS; ++id)
		EXPECT_TRUE(manager.contains(id)) << "id " << id << " should survive";
	EXPECT_FALSE(manager.contains(ORDERS - CAPACITY));
}

// A live order sitting among retired ones must not be recycled just because it
// is the oldest thing in the table — only the retired FIFO is eligible.
TEST(OrderManagerRecycling, ALiveOrderIsNeverRecycledHoweverOldItIs) {
	order_manager manager{3};
	const auto resting = manager.admit(limit(1)); // never terminal
	ASSERT_TRUE(resting.has_value());

	for (order_id_t id = 2; id <= 40; ++id) {
		const auto handle = manager.admit(limit(id));
		ASSERT_TRUE(handle.has_value()) << "refused order " << id;
		manager.cancel(*handle);
	}

	EXPECT_TRUE(manager.contains(1));
	const order_record *record = manager.get(*resting);
	ASSERT_NE(record, nullptr) << "the oldest record, and still live";
	EXPECT_EQ(record->status(), OrderStatus::LIVE);
	EXPECT_EQ(manager.live(), 1u);
}

TEST(OrderManagerRecycling, ClearForgetsEveryOrderAndStalesEveryHandle) {
	order_manager manager{8};
	const auto live   = manager.admit(limit(1));
	const auto filled = manager.admit(limit(2, 10));
	ASSERT_TRUE(live.has_value());
	ASSERT_TRUE(filled.has_value());
	manager.apply_fill(*filled, 10);

	manager.clear();

	EXPECT_EQ(manager.live(), 0u);
	EXPECT_EQ(manager.retained(), 0u);
	EXPECT_EQ(manager.size(), 0u);
	EXPECT_FALSE(manager.contains(1));
	EXPECT_EQ(manager.get(*live), nullptr);
	EXPECT_EQ(manager.get(*filled), nullptr);
	// A lifetime capacity reading, not a per-session one: it survives on purpose.
	EXPECT_EQ(manager.high_water(), 2u);
	EXPECT_EQ(manager.capacity(), 8u);

	// And the manager is reusable, including for the ids it just forgot.
	const auto again = manager.admit(limit(1));
	ASSERT_TRUE(again.has_value());
	EXPECT_NE(manager.get(*again), nullptr);
	EXPECT_EQ(manager.get(*live), nullptr) << "clear must not resurrect a handle";
}
