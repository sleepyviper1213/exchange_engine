// The other direction: what the gate does with the trades and outcomes the
// partition publishes back. Getting this wrong is the failure mode a risk
// system must not have quietly — an order that is retired twice makes every
// later exposure check too permissive.

#include "gate.fixture.hpp"
#include "trading-engine/order_book/order_state.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/reject_reason.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/risk/limits.hpp"

#include <gtest/gtest.h>

namespace {

using exchange::order_id_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::order_outcome;
using exchange::engine::OrderStatus;
using exchange::engine::OutcomeType;
using exchange::engine::reject_reason;
using exchange::engine::risk::risk_limits;

using exchange::test::risk::buy;
using exchange::test::risk::harness;
using exchange::test::risk::permissive;
using exchange::test::risk::sell;
using exchange::test::risk::SYMBOL;

/// @brief The outcome the book emits when an order is withdrawn with @p left
///        still unexecuted.
[[nodiscard]] order_outcome cancelled(order_id_t id, quantity_t traded,
									  quantity_t left) {
	return {.id        = id,
			.type      = OutcomeType::CANCELLED,
			.reason    = reject_reason::NONE,
			.status    = OrderStatus::CANCELLED,
			.traded    = traded,
			.remaining = left};
}

TEST(RiskGateFeedback, AFillMovesThePositionAndRetiresTheWorkingQuantity) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	ASSERT_EQ(h.working(side_t::bid), 10);

	h.filled(/*aggressor=*/1, /*resting=*/999, /*price=*/100, /*volume=*/10);

	EXPECT_EQ(h.net(), 10);
	EXPECT_EQ(h.working(side_t::bid), 0);
	EXPECT_EQ(h.gate().working_orders(), 0U);
}

TEST(RiskGateFeedback, AFillIsMarkedAtTheExecutionPriceAndNotTheLimit) {
	// A trade prints at the resting order's price. Marking an aggressive buy at
	// its own limit would overstate what it paid on every one.
	harness h;
	ASSERT_TRUE(h.place(buy(1, 110, 10)));
	h.filled(1, 999, /*price=*/100, 10);
	EXPECT_EQ(h.positions().snapshot(SYMBOL).net_notional, 1000);
}

TEST(RiskGateFeedback, APartialFillLeavesTheRemainderWorking) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	h.filled(1, 999, 100, 4);

	EXPECT_EQ(h.net(), 4);
	EXPECT_EQ(h.working(side_t::bid), 6);
	EXPECT_EQ(h.gate().ledger().find(1)->lots, 6);
}

TEST(RiskGateFeedback, ARestingOrderOfOursIsRecognisedToo) {
	// A trade names two ids and either can be ours; the gate looks up both.
	harness h;
	ASSERT_TRUE(h.place(sell(1, 100, 5)));
	h.filled(/*aggressor=*/999, /*resting=*/1, 100, 5);

	EXPECT_EQ(h.net(), -5);
	EXPECT_EQ(h.working(side_t::ask), 0);
}

TEST(RiskGateFeedback, ASelfTradeNetsToZeroWithoutASpecialCase) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 5)));
	ASSERT_TRUE(h.place(sell(2, 100, 5)));

	h.filled(/*aggressor=*/1, /*resting=*/2, 100, 5);

	EXPECT_EQ(h.net(), 0);
	EXPECT_EQ(h.working(side_t::bid), 0);
	EXPECT_EQ(h.working(side_t::ask), 0);
	EXPECT_EQ(h.gate().working_orders(), 0U);
}

TEST(RiskGateFeedback, ATradeInSomebodyElsesOrdersChangesNothing) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	h.filled(500, 501, 100, 7);

	EXPECT_EQ(h.net(), 0);
	EXPECT_EQ(h.working(side_t::bid), 10);
}

TEST(RiskGateFeedback, ACancelRetiresWhatIsLeftWorking) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	h.filled(1, 999, 100, 4);
	ASSERT_EQ(h.working(side_t::bid), 6);

	h.outcome(cancelled(1, /*traded=*/4, /*left=*/6));

	EXPECT_EQ(h.working(side_t::bid), 0);
	EXPECT_EQ(h.gate().working_orders(), 0U);
	// The four that filled stay filled.
	EXPECT_EQ(h.net(), 4);
}

TEST(RiskGateFeedback, ARejectionRetiresTheWholeOrder) {
	// The gate counted the exposure at submission; the book then refused it, so
	// none of it was ever real.
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	h.outcome(order_outcome::rejected(1, reject_reason::BOOK_AT_CAPACITY, 10));

	EXPECT_EQ(h.working(side_t::bid), 0);
	EXPECT_EQ(h.gate().working_orders(), 0U);
	EXPECT_EQ(h.net(), 0);
}

TEST(RiskGateFeedback, AFillFollowedByItsTerminalOutcomeRetiresOnlyOnce) {
	// The partition publishes trades before outcomes, so a fully filled order
	// arrives twice: once as the trade that moved it and once as the FILL that
	// closed it. Retiring on both would take working quantity negative and make
	// every later exposure check too permissive.
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	h.filled(1, 999, 100, 10);
	h.outcome(order_outcome{.id        = 1,
							.type      = OutcomeType::FILL,
							.reason    = reject_reason::NONE,
							.status    = OrderStatus::FILLED,
							.traded    = 10,
							.remaining = 0});

	EXPECT_EQ(h.working(side_t::bid), 0);
	EXPECT_EQ(h.net(), 10);
}

TEST(RiskGateFeedback, AnAcknowledgementChangesNothing) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	h.outcome(order_outcome::accepted(1, 10));

	EXPECT_EQ(h.working(side_t::bid), 10);
	EXPECT_EQ(h.gate().working_orders(), 1U);
}

TEST(RiskGateFeedback, AnOutcomeForAnUnknownOrderIsIgnored) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	h.outcome(cancelled(77, 0, 5));

	EXPECT_EQ(h.working(side_t::bid), 10);
	EXPECT_EQ(h.gate().working_orders(), 1U);
}

TEST(RiskGateFeedback, ATradeRemarksTheFatFingerBand) {
	risk_limits limits    = permissive();
	limits.price_band_bps = 500;
	harness h{limits, /*reference=*/1000};
	ASSERT_EQ(h.gate().band_high(), 1050U);

	h.filled(500, 501, /*price=*/2000, 1);

	EXPECT_EQ(h.gate().reference_price(), 2000U);
	EXPECT_EQ(h.gate().band_low(), 1900U);
	EXPECT_EQ(h.gate().band_high(), 2100U);
	// A price that was outside the old band and is inside the new one.
	ASSERT_TRUE(h.place(buy(1, 2050, 1)));
	EXPECT_EQ(h.delivered().size(), 1U);
}

TEST(RiskGateFeedback, TheBandCanBeSeededBeforeAnyTrade) {
	risk_limits limits    = permissive();
	limits.price_band_bps = 200;
	harness h{limits};
	// No reference yet, so nothing is out of band.
	ASSERT_TRUE(h.place(buy(1, 9999, 1)));
	ASSERT_EQ(h.delivered().size(), 1U);

	h.gate().set_reference_price(1000);
	ASSERT_TRUE(h.place(buy(2, 9999, 1)));
	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_PRICE_BAND);
}

TEST(RiskGateFeedback, TheBandFloorNeverFallsBelowOneTick) {
	// A price of zero ticks is never admissible, so a band wider than the mark
	// still has a floor.
	risk_limits limits    = permissive();
	limits.price_band_bps = 50000; // ±500%
	harness h{limits, /*reference=*/10};
	EXPECT_EQ(h.gate().band_low(), 1U);
}

TEST(RiskGateFeedback, RetiringWorkingQuantityFreesRoomUnderTheExposureLimit) {
	// The end-to-end shape: exposure blocks, a fill and a cancel clear it, and
	// the next order gets through.
	risk_limits limits           = permissive();
	limits.max_exposure_notional = 100 * 20;
	harness h{limits, /*reference=*/100};

	ASSERT_TRUE(h.place(buy(1, 100, 20)));
	ASSERT_TRUE(h.place(buy(2, 100, 5)));
	ASSERT_EQ(h.delivered().size(), 1U);
	ASSERT_EQ(h.sole_rejection().reason, reject_reason::RISK_EXPOSURE_LIMIT);

	h.outcome(cancelled(1, 0, 20));
	ASSERT_TRUE(h.place(buy(3, 100, 5)));
	EXPECT_EQ(h.delivered().size(), 2U);
}

} // namespace
