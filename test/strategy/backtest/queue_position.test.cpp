#include "strategy/backtest/queue_position.hpp"

#include <gtest/gtest.h>

// The queue-position estimate on its own, away from the fill model that drives
// it. Every case is a statement about what the estimate may conclude from a
// depth feed - the join is exact, the tightening is a deduction, and nothing
// else is allowed to move it.

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

constexpr price_t PRICE = 100;

} // namespace

TEST(BacktestQueuePosition, KnowsNothingAboutAPriceWeDoNotHold) {
	queue_position_book queue;
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 0);
	EXPECT_EQ(queue.tracked(), 0U);
}

// The join is exact rather than modelled: everything published at our price
// when we arrive got there before us, and nothing has arrived after us yet.
TEST(BacktestQueuePosition, JoinsBehindEverythingPublishedAtOurPrice) {
	queue_position_book queue;
	queue.open_side(side_t::bid);
	queue.track(side_t::bid, PRICE, 40);
	queue.close_side(side_t::bid);

	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 40);
	EXPECT_EQ(queue.tracked(), 1U);
}

TEST(BacktestQueuePosition, JoinsAtTheFrontOfAPriceNobodyIsQuoting) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 0);
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 0)
		<< "no published size means no queue, which is the one case where "
		   "being first is an observation rather than an assumption";
}

TEST(BacktestQueuePosition, KeepsTheTwoSidesApart) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);
	queue.track(side_t::ask, PRICE, 7);

	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 40);
	EXPECT_EQ(queue.ahead(side_t::ask, PRICE), 7);
	EXPECT_EQ(queue.tracked(), 2U);
}

// The published size is `ahead + behind`, so a later observation that is larger
// says nothing about the queue in front of us - it is people arriving behind.
TEST(BacktestQueuePosition, IgnoresLiquidityThatJoinedBehindUs) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);
	queue.track(side_t::bid, PRICE, 90);

	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 40)
		<< "a growing level grows at the back";
}

// And the deduction that runs the other way: `behind` is never negative, so a
// published size below the queue we recorded proves the queue itself shrank.
TEST(BacktestQueuePosition, TightensTheEstimateToWhatIsStillPublished) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);
	queue.track(side_t::bid, PRICE, 15);

	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 15);
}

TEST(BacktestQueuePosition, NeverLetsTheEstimateGoBackUp) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);
	queue.track(side_t::bid, PRICE, 15);
	queue.track(side_t::bid, PRICE, 60);

	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 15)
		<< "the tightening is a floor on our knowledge, not a running "
		   "readout of the level";
}

TEST(BacktestQueuePosition, TreatsANegativePublishedSizeAsEmpty) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, -5);
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 0);
}

TEST(BacktestQueuePosition, AbsorbsVolumeIntoTheQueueAheadOfUs) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);

	EXPECT_EQ(queue.absorb(side_t::bid, PRICE, 10), 10);
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 30);
	EXPECT_EQ(queue.absorbed_lots(), 10);
}

// The part the caller needs back: how much did *not* fit in the queue, and
// therefore reaches our own orders.
TEST(BacktestQueuePosition, AbsorbsNoMoreThanTheQueueThatWasThere) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 8);

	EXPECT_EQ(queue.absorb(side_t::bid, PRICE, 30), 8)
		<< "the remaining 22 is the caller's, and is what fills us";
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 0);
}

TEST(BacktestQueuePosition, AbsorbsNothingOnceWeAreAtTheFront) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 5);
	ASSERT_EQ(queue.absorb(side_t::bid, PRICE, 5), 5);

	EXPECT_EQ(queue.absorb(side_t::bid, PRICE, 100), 0);
	EXPECT_EQ(queue.absorbed_lots(), 5);
}

TEST(BacktestQueuePosition, AbsorbsNothingAtAPriceWeDoNotHold) {
	queue_position_book queue;
	EXPECT_EQ(queue.absorb(side_t::bid, PRICE, 10), 0);
	EXPECT_EQ(queue.absorbed_lots(), 0);
}

TEST(BacktestQueuePosition, IgnoresANonPositiveAbsorption) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);

	EXPECT_EQ(queue.absorb(side_t::bid, PRICE, 0), 0);
	EXPECT_EQ(queue.absorb(side_t::bid, PRICE, -3), 0);
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 40);
}

// Progress is the whole point: what a resting order paid down stays paid down,
// so it works its way forward over successive events.
TEST(BacktestQueuePosition, KeepsProgressAcrossReconciliations) {
	queue_position_book queue;
	queue.open_side(side_t::bid);
	queue.track(side_t::bid, PRICE, 40);
	queue.close_side(side_t::bid);
	ASSERT_EQ(queue.absorb(side_t::bid, PRICE, 25), 25);

	queue.open_side(side_t::bid);
	queue.track(side_t::bid, PRICE, 40);
	queue.close_side(side_t::bid);

	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 15)
		<< "the level being republished at its old size must not put us back "
		   "where we started";
}

// The reason open_side/close_side exist. Leaving a price and returning must
// re-measure, or the second visit inherits a position the first one paid for.
TEST(BacktestQueuePosition, ForgetsAPriceWeHaveLeft) {
	queue_position_book queue;
	queue.open_side(side_t::bid);
	queue.track(side_t::bid, PRICE, 40);
	queue.close_side(side_t::bid);
	ASSERT_EQ(queue.absorb(side_t::bid, PRICE, 40), 40);

	queue.open_side(side_t::bid); // we hold nothing this time round
	queue.close_side(side_t::bid);
	EXPECT_EQ(queue.tracked(), 0U);

	queue.track(side_t::bid, PRICE, 40);
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 40)
		<< "coming back to a price is joining it again, at the back";
}

TEST(BacktestQueuePosition, KeepsAPriceWeStillHold) {
	queue_position_book queue;
	queue.open_side(side_t::bid);
	queue.track(side_t::bid, PRICE, 40);
	queue.track(side_t::bid, 99, 12);
	queue.close_side(side_t::bid);

	queue.open_side(side_t::bid);
	queue.track(side_t::bid, PRICE, 40);
	queue.close_side(side_t::bid);

	EXPECT_EQ(queue.tracked(), 1U) << "only the price we stopped quoting goes";
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 40);
}

TEST(BacktestQueuePosition, ReconcilingOneSideLeavesTheOtherAlone) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);
	queue.track(side_t::ask, PRICE, 7);

	queue.open_side(side_t::bid);
	queue.close_side(side_t::bid);

	EXPECT_EQ(queue.tracked(), 1U);
	EXPECT_EQ(queue.ahead(side_t::ask, PRICE), 7);
}

// A gap invalidates every estimate at once, because every one of them was
// measured against the replica that just died.
TEST(BacktestQueuePosition, ClearAbandonsEveryEstimate) {
	queue_position_book queue;
	queue.track(side_t::bid, PRICE, 40);
	queue.track(side_t::ask, PRICE, 7);
	ASSERT_EQ(queue.absorb(side_t::bid, PRICE, 10), 10);

	queue.clear();

	EXPECT_EQ(queue.tracked(), 0U);
	EXPECT_EQ(queue.ahead(side_t::bid, PRICE), 0);
	EXPECT_EQ(queue.absorbed_lots(), 10)
		<< "what was absorbed is a fact about the run and survives; only the "
		   "estimates it was derived from are void";
}
