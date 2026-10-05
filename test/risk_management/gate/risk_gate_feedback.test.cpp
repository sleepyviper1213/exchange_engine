// The other direction: what the gate does with the trades and outcomes the
// partition publishes back. Getting this wrong is the failure mode a risk
// system must not have quietly - an order that is retired twice makes every
// later exposure check too permissive.

#include "gate.fixture.hpp"
#include "order_book/order_status.hpp"
#include "order_book/outcome.hpp"
#include "order_book/outcome_type.hpp"
#include "order_book/reject_reason.hpp"

#include <gtest/gtest.h>

namespace {

using namespace exchange;

using exchange::engine::order_outcome;
using exchange::engine::OrderStatus;
using exchange::engine::OutcomeType;
using exchange::engine::reject_reason;

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
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));
	ASSERT_EQ(h.working(side_t::bid), 10 * units::lot);

	h.filled(/*aggressor=*/1,
			 /*resting=*/999,
			 /*price=*/at_tick(100),
			 /*volume=*/10 * units::lot);

	EXPECT_EQ(h.net(), 10 * units::lot);
	EXPECT_EQ(h.working(side_t::bid), 0 * units::lot);
	EXPECT_EQ(h.gate().working_orders(), 0U);
}

TEST(RiskGateFeedback, AFillIsMarkedAtTheExecutionPriceAndNotTheLimit) {
	// A trade prints at the resting order's price. Marking an aggressive buy at
	// its own limit would overstate what it paid on every one.
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(110), 10 * units::lot)));
	h.filled(1, 999, /*price=*/at_tick(100), 10 * units::lot);
	EXPECT_EQ(h.positions().snapshot(SYMBOL).net_notional,
			  1000 * (units::tick * units::lot));
}

TEST(RiskGateFeedback, APartialFillLeavesTheRemainderWorking) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));
	h.filled(1, 999, at_tick(100), 4 * units::lot);

	EXPECT_EQ(h.net(), 4 * units::lot);
	EXPECT_EQ(h.working(side_t::bid), 6 * units::lot);
	EXPECT_EQ(h.gate().ledger().find(1)->lots, 6 * units::lot);
}

TEST(RiskGateFeedback, ARestingOrderOfOursIsRecognisedToo) {
	// A trade names two ids and either can be ours; the gate looks up both.
	harness h;
	ASSERT_TRUE(h.place(sell(1, at_tick(100), 5 * units::lot)));
	h.filled(/*aggressor=*/999, /*resting=*/1, at_tick(100), 5 * units::lot);

	EXPECT_EQ(h.net(), -5 * units::lot);
	EXPECT_EQ(h.working(side_t::ask), 0 * units::lot);
}

TEST(RiskGateFeedback, ASelfTradeNetsToZeroWithoutASpecialCase) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 5 * units::lot)));
	ASSERT_TRUE(h.place(sell(2, at_tick(100), 5 * units::lot)));

	h.filled(/*aggressor=*/1, /*resting=*/2, at_tick(100), 5 * units::lot);

	EXPECT_EQ(h.net(), 0 * units::lot);
	EXPECT_EQ(h.working(side_t::bid), 0 * units::lot);
	EXPECT_EQ(h.working(side_t::ask), 0 * units::lot);
	EXPECT_EQ(h.gate().working_orders(), 0U);
}

TEST(RiskGateFeedback, ATradeInSomebodyElsesOrdersChangesNothing) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));
	h.filled(500, 501, at_tick(100), 7 * units::lot);

	EXPECT_EQ(h.net(), 0 * units::lot);
	EXPECT_EQ(h.working(side_t::bid), 10 * units::lot);
}

TEST(RiskGateFeedback, ACancelRetiresWhatIsLeftWorking) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));
	h.filled(1, 999, at_tick(100), 4 * units::lot);
	ASSERT_EQ(h.working(side_t::bid), 6 * units::lot);

	h.outcome(cancelled(1, /*traded=*/4 * units::lot, /*left=*/6 * units::lot));

	EXPECT_EQ(h.working(side_t::bid), 0 * units::lot);
	EXPECT_EQ(h.gate().working_orders(), 0U);
	// The four that filled stay filled.
	EXPECT_EQ(h.net(), 4 * units::lot);
}

TEST(RiskGateFeedback, ARejectionRetiresTheWholeOrder) {
	// The gate counted the exposure at submission; the book then refused it, so
	// none of it was ever real.
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));

	h.outcome(order_outcome::rejected(1, reject_reason::BOOK_AT_CAPACITY, 10 * units::lot));

	EXPECT_EQ(h.working(side_t::bid), 0 * units::lot);
	EXPECT_EQ(h.gate().working_orders(), 0U);
	EXPECT_EQ(h.net(), 0 * units::lot);
}

TEST(RiskGateFeedback, AFillFollowedByItsTerminalOutcomeRetiresOnlyOnce) {
	// The partition publishes trades before outcomes, so a fully filled order
	// arrives twice: once as the trade that moved it and once as the FILL that
	// closed it. Retiring on both would take working quantity negative and make
	// every later exposure check too permissive.
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));

	h.filled(1, 999, at_tick(100), 10 * units::lot);
	h.outcome(order_outcome{.id        = 1,
							.type      = OutcomeType::FILL,
							.reason    = reject_reason::NONE,
							.status    = OrderStatus::FILLED,
							.traded    = 10 * units::lot,
							.remaining = 0 * units::lot});

	EXPECT_EQ(h.working(side_t::bid), 0 * units::lot);
	EXPECT_EQ(h.net(), 10 * units::lot);
}

TEST(RiskGateFeedback, AnAcknowledgementChangesNothing) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));
	h.outcome(order_outcome::accepted(1, 10 * units::lot));

	EXPECT_EQ(h.working(side_t::bid), 10 * units::lot);
	EXPECT_EQ(h.gate().working_orders(), 1U);
}

TEST(RiskGateFeedback, AnOutcomeForAnUnknownOrderIsIgnored) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, at_tick(100), 10 * units::lot)));
	h.outcome(cancelled(77, {}, 5 * units::lot));

	EXPECT_EQ(h.working(side_t::bid), 10 * units::lot);
	EXPECT_EQ(h.gate().working_orders(), 1U);
}

TEST(RiskGateFeedback, ATradeRemarksTheFatFingerBand) {
	risk_limits limits    = permissive();
	limits.price_band_bps = 500;
	harness h{limits, /*reference=*/at_tick(1000)};
	ASSERT_EQ(h.gate().band_high(), at_tick(1050));

	h.filled(500, 501, /*price=*/at_tick(2000), 1 * units::lot);

	EXPECT_EQ(h.gate().reference_price(), at_tick(2000));
	EXPECT_EQ(h.gate().band_low(), at_tick(1900));
	EXPECT_EQ(h.gate().band_high(), at_tick(2100));
	// A price that was outside the old band and is inside the new one.
	ASSERT_TRUE(h.place(buy(1, at_tick(2050), 1 * units::lot)));
	EXPECT_EQ(h.delivered().size(), 1U);
}

TEST(RiskGateFeedback, TheBandCanBeSeededBeforeAnyTrade) {
	risk_limits limits    = permissive();
	limits.price_band_bps = 200;
	harness h{limits};
	// No reference yet, so nothing is out of band.
	ASSERT_TRUE(h.place(buy(1, at_tick(9999), 1 * units::lot)));
	ASSERT_EQ(h.delivered().size(), 1U);

	h.gate().set_reference_price(at_tick(1000));
	ASSERT_TRUE(h.place(buy(2, at_tick(9999), 1 * units::lot)));
	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_PRICE_BAND);
}

TEST(RiskGateFeedback, TheBandFloorNeverFallsBelowOneTick) {
	// A price of zero ticks is never admissible, so a band wider than the mark
	// still has a floor.
	risk_limits limits    = permissive();
	limits.price_band_bps = 50000; // ±500%
	harness h{limits, /*reference=*/at_tick(10)};
	EXPECT_EQ(h.gate().band_low(), at_tick(1));
}

TEST(RiskGateFeedback, RetiringWorkingQuantityFreesRoomUnderTheExposureLimit) {
	// The end-to-end shape: exposure blocks, a fill and a cancel clear it, and
	// the next order gets through.
	risk_limits limits           = permissive();
	limits.max_exposure_notional = 100 * 20 * (units::tick * units::lot);
	harness h{limits, /*reference=*/at_tick(100)};

	ASSERT_TRUE(h.place(buy(1, at_tick(100), 20 * units::lot)));
	ASSERT_TRUE(h.place(buy(2, at_tick(100), 5 * units::lot)));
	ASSERT_EQ(h.delivered().size(), 1U);
	ASSERT_EQ(h.sole_rejection().reason, reject_reason::RISK_EXPOSURE_LIMIT);

	h.outcome(cancelled(1, {}, 20 * units::lot));
	ASSERT_TRUE(h.place(buy(3, at_tick(100), 5 * units::lot)));
	EXPECT_EQ(h.delivered().size(), 2U);
}

} // namespace
