#include "venue/weight_budget.hpp"

#include <gtest/gtest.h>

#include <chrono>

// The rolling rate-limit allowance. What is being pinned is that the window
// actually rolls - a budget that only ever accumulates would refuse traffic for
// ever after one busy minute, and one that never ages anything out would walk
// the address into a 418.

using exchange::venue::BINANCE_SPOT_WEIGHT_PER_MINUTE;
using exchange::venue::weight_budget;

namespace {

using budget_clock = weight_budget::clock;

/// A fixed origin, so every case reads in seconds from a known zero rather than
/// from whatever the steady clock happened to say.
const budget_clock::time_point WEIGHT_EPOCH{};

[[nodiscard]] budget_clock::time_point weight_at(int seconds) {
	return WEIGHT_EPOCH + std::chrono::seconds{seconds};
}

/// A small window, so a test can walk past the end of it in a few lines.
[[nodiscard]] weight_budget weight_small_budget() {
	return weight_budget{100, std::chrono::seconds{10}};
}

} // namespace

TEST(WeightBudget, SpendingReducesWhatIsLeft) {
	weight_budget budget = weight_small_budget();

	budget.spend(30, weight_at(0));
	EXPECT_EQ(budget.used(weight_at(0)), 30);
	EXPECT_EQ(budget.remaining(weight_at(0)), 70);
}

TEST(WeightBudget, SpendWithinOneSecondAccumulates) {
	weight_budget budget = weight_small_budget();

	budget.spend(10, weight_at(0));
	budget.spend(15, weight_at(0));
	EXPECT_EQ(budget.used(weight_at(0)), 25);
}

TEST(WeightBudget, SpendLeavesTheWindowOnceItIsOlderThanIt) {
	weight_budget budget = weight_small_budget();

	budget.spend(100, weight_at(0));
	ASSERT_EQ(budget.used(weight_at(0)), 100);

	// Still inside at the last second of the window...
	EXPECT_EQ(budget.used(weight_at(9)), 100);
	// ...and gone once the window has moved past the second it was spent in.
	EXPECT_EQ(budget.used(weight_at(10)), 0);
	EXPECT_EQ(budget.remaining(weight_at(10)), 100);
}

TEST(WeightBudget, OnlyTheSpendThatAgedOutIsReturned) {
	weight_budget budget = weight_small_budget();

	budget.spend(40, weight_at(0));
	budget.spend(30, weight_at(5));
	ASSERT_EQ(budget.used(weight_at(5)), 70);

	// The first second has left the window; the sixth has not.
	EXPECT_EQ(budget.used(weight_at(10)), 30);
	// And at second 15 the later spend has aged out too.
	EXPECT_EQ(budget.used(weight_at(15)), 0);
}

TEST(WeightBudget, AGapLongerThanTheWindowClearsEverything) {
	weight_budget budget = weight_small_budget();

	budget.spend(100, weight_at(0));
	// The whole ring is stale, so this is a reset rather than ten evictions -
	// and a quiet process must not come back still throttled.
	EXPECT_EQ(budget.used(weight_at(10000)), 0);
}

TEST(WeightBudget, CanSpendRefusesWhatWouldCrossTheLimit) {
	weight_budget budget = weight_small_budget();

	budget.spend(95, weight_at(0));
	EXPECT_TRUE(budget.can_spend(5, weight_at(0)));
	// Exactly at the limit is admissible; one past it is not.
	EXPECT_FALSE(budget.can_spend(6, weight_at(0)));
}

TEST(WeightBudget, ReconcileAdoptsTheVenuesCountOverOurEstimate) {
	weight_budget budget = weight_small_budget();

	budget.spend(10, weight_at(0));
	// The venue says 80, which is what a retry, another process on this address
	// or a weight we guessed wrong looks like. Its number wins.
	budget.reconcile(80, weight_at(1));

	EXPECT_EQ(budget.used(weight_at(1)), 80);
	EXPECT_EQ(budget.remaining(weight_at(1)), 20);
}

TEST(WeightBudget, AReconciledTotalAgesOutLikeAnySpend) {
	weight_budget budget = weight_small_budget();

	budget.reconcile(80, weight_at(0));
	ASSERT_EQ(budget.used(weight_at(0)), 80);
	// A whole window later it is gone; the adopted figure is not permanent.
	EXPECT_EQ(budget.used(weight_at(10)), 0);
}

TEST(WeightBudget, TimeGoingBackwardsDoesNotUnspend) {
	weight_budget budget = weight_small_budget();

	budget.spend(50, weight_at(5));
	// An out-of-order timestamp is a caller reporting late, not evidence that
	// the spend never happened. Un-ageing it would be how a budget drifts under
	// the venue's own count.
	EXPECT_EQ(budget.used(weight_at(0)), 50);
}

TEST(WeightBudget, ANonPositiveLimitImposesNoCeiling) {
	weight_budget budget{0, std::chrono::seconds{10}};

	EXPECT_TRUE(budget.is_unlimited());
	budget.spend(1'000'000, weight_at(0));
	EXPECT_TRUE(budget.can_spend(1'000'000, weight_at(0)));
}

TEST(WeightBudget, TheDefaultIsBinancesDocumentedSpotAllowance) {
	const weight_budget budget;

	EXPECT_EQ(budget.limit(), BINANCE_SPOT_WEIGHT_PER_MINUTE);
	EXPECT_EQ(budget.window(), std::chrono::seconds{60});
	EXPECT_FALSE(budget.is_unlimited());
}
