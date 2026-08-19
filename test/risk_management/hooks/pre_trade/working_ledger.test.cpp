// The open-addressed table of orders in flight. The interesting test is the
// last one: backward-shift deletion is the whole reason this is not a
// tombstoned table, and a broken shift shows up as an entry that silently
// stops being found.

#include "risk_management/hooks/pre_trade/working_ledger.hpp"

#include "trading-engine/orders/types.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using exchange::side_t;
using exchange::risk::working_ledger;

TEST(RiskWorkingLedger, AFreshLedgerIsEmptyAndSizedToItsLimit) {
	const working_ledger ledger{10};
	EXPECT_TRUE(ledger.is_empty());
	EXPECT_EQ(ledger.size(), 0U);
	EXPECT_EQ(ledger.limit(), 10U);
	EXPECT_FALSE(ledger.is_full());
	// Over-allocated to keep the load factor near 0.7, and a power of two.
	EXPECT_GT(ledger.slot_count(), 10U);
	EXPECT_EQ(ledger.slot_count() & (ledger.slot_count() - 1), 0U);
}

TEST(RiskWorkingLedger, AnInsertedOrderIsFoundWithItsFields) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(42, side_t::ask, 1250, 7));

	const auto found = ledger.find(42);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->id, 42U);
	EXPECT_EQ(found->side, side_t::ask);
	EXPECT_EQ(found->price, 1250U);
	EXPECT_EQ(found->lots, 7);
	EXPECT_EQ(ledger.size(), 1U);
}

TEST(RiskWorkingLedger, TheSideSurvivesBeingFoldedIntoTheQuantitySSign) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 5));
	ASSERT_TRUE(ledger.insert(2, side_t::ask, 100, 5));
	EXPECT_EQ(ledger.find(1)->side, side_t::bid);
	EXPECT_EQ(ledger.find(2)->side, side_t::ask);
	EXPECT_EQ(ledger.find(1)->lots, 5);
	EXPECT_EQ(ledger.find(2)->lots, 5);
}

TEST(RiskWorkingLedger, AnIdAlreadyTrackedIsRefused) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 5));
	EXPECT_FALSE(ledger.insert(1, side_t::ask, 200, 9));
	// And the original is untouched.
	EXPECT_EQ(ledger.find(1)->price, 100U);
	EXPECT_EQ(ledger.size(), 1U);
}

TEST(RiskWorkingLedger, TheReservedZeroIdIsNeverTracked) {
	working_ledger ledger{10};
	EXPECT_FALSE(ledger.insert(0, side_t::bid, 100, 5));
	EXPECT_FALSE(ledger.contains(0));
	EXPECT_FALSE(ledger.find(0).has_value());
	EXPECT_TRUE(ledger.is_empty());
}

TEST(RiskWorkingLedger, ANonPositiveQuantityIsRefused) {
	working_ledger ledger{10};
	EXPECT_FALSE(ledger.insert(1, side_t::bid, 100, 0));
	EXPECT_FALSE(ledger.insert(2, side_t::bid, 100, -5));
	EXPECT_TRUE(ledger.is_empty());
}

TEST(RiskWorkingLedger, TheLimitIsTheLimitEvenThoughTheTableIsLarger) {
	working_ledger ledger{3};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 1));
	ASSERT_TRUE(ledger.insert(2, side_t::bid, 100, 1));
	ASSERT_TRUE(ledger.insert(3, side_t::bid, 100, 1));
	EXPECT_TRUE(ledger.is_full());
	EXPECT_FALSE(ledger.insert(4, side_t::bid, 100, 1));
	EXPECT_EQ(ledger.size(), 3U);
}

TEST(RiskWorkingLedger, APartialTakeLeavesTheRestWorking) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 10));

	const auto taken = ledger.take(1, 4);
	ASSERT_TRUE(taken.has_value());
	EXPECT_EQ(taken->taken, 4);
	EXPECT_EQ(taken->remaining, 6);
	EXPECT_EQ(taken->side, side_t::bid);
	EXPECT_EQ(taken->price, 100U);
	EXPECT_EQ(ledger.find(1)->lots, 6);
}

TEST(RiskWorkingLedger, TakingTheLastLotErasesTheEntry) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 10));

	const auto taken = ledger.take(1, 10);
	ASSERT_TRUE(taken.has_value());
	EXPECT_EQ(taken->remaining, 0);
	EXPECT_FALSE(ledger.contains(1));
	EXPECT_TRUE(ledger.is_empty());
}

TEST(RiskWorkingLedger, ATakeLargerThanWhatIsWorkingIsClamped) {
	// A fill bigger than the ledger thinks is out there means the ledger missed
	// something; going negative would corrupt every later exposure check rather
	// than only this one.
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 3));

	const auto taken = ledger.take(1, 99);
	ASSERT_TRUE(taken.has_value());
	EXPECT_EQ(taken->taken, 3);
	EXPECT_EQ(taken->remaining, 0);
	EXPECT_FALSE(ledger.contains(1));
}

TEST(RiskWorkingLedger, TakingFromAnUnknownIdReportsNothing) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 3));
	EXPECT_FALSE(ledger.take(99, 1).has_value());
	EXPECT_FALSE(ledger.retire(99).has_value());
	EXPECT_EQ(ledger.size(), 1U);
}

TEST(RiskWorkingLedger, RetiringReturnsEverythingStillWorking) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::ask, 250, 8));
	ASSERT_TRUE(ledger.take(1, 3).has_value());

	const auto retired = ledger.retire(1);
	ASSERT_TRUE(retired.has_value());
	EXPECT_EQ(retired->taken, 5);
	EXPECT_EQ(retired->remaining, 0);
	EXPECT_EQ(retired->side, side_t::ask);
	EXPECT_FALSE(ledger.contains(1));
}

TEST(RiskWorkingLedger, AnErasedSlotDoesNotHideTheEntriesBehindIt) {
	// Backward-shift deletion, the reason this table has no tombstones. Fill it
	// densely so probe chains genuinely overlap, then erase from the middle of
	// each chain and check that nothing became unreachable.
	constexpr std::uint32_t COUNT = 200;
	working_ledger ledger{COUNT};
	for (std::uint32_t i = 1; i <= COUNT; ++i)
		ASSERT_TRUE(ledger.insert(i, side_t::bid, 100 + i, 1))
			<< "insert " << i;

	// Erase every third id, then confirm every surviving id is still found with
	// the right payload - a broken shift orphans whichever entries probed
	// through the hole.
	for (std::uint32_t i = 1; i <= COUNT; i += 3) ASSERT_TRUE(ledger.retire(i));

	for (std::uint32_t i = 1; i <= COUNT; ++i) {
		const auto found = ledger.find(i);
		if (i % 3 == 1) {
			EXPECT_FALSE(found.has_value()) << "id " << i << " should be gone";
		} else {
			ASSERT_TRUE(found.has_value()) << "id " << i << " went missing";
			EXPECT_EQ(found->price, 100 + i);
		}
	}
	EXPECT_EQ(ledger.size(), COUNT - ((COUNT + 2) / 3));
}

TEST(RiskWorkingLedger, ReinsertingAfterAnEraseReusesTheSpace) {
	// A tombstoned table would fill up here; this one should not.
	working_ledger ledger{4};
	for (std::uint64_t round = 0; round < 1000; ++round) {
		ASSERT_TRUE(ledger.insert(round + 1, side_t::bid, 100, 1))
			<< "round " << round;
		ASSERT_TRUE(ledger.retire(round + 1));
	}
	EXPECT_TRUE(ledger.is_empty());
}

TEST(RiskWorkingLedger, ClearForgetsEverything) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(1, side_t::bid, 100, 1));
	ASSERT_TRUE(ledger.insert(2, side_t::bid, 100, 1));
	ledger.clear();
	EXPECT_TRUE(ledger.is_empty());
	EXPECT_FALSE(ledger.contains(1));
	EXPECT_TRUE(ledger.insert(1, side_t::bid, 100, 1));
}

} // namespace
