// One rule per test. Each starts from limits that refuse nothing and tightens
// exactly one field, so a failure names the rule that broke rather than the
// scenario that reached it.

#include "gate.fixture.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/reject_reason.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/risk/breach.hpp"

#include <gtest/gtest.h>

namespace {

using exchange::order_id_t;
using exchange::side_t;
using exchange::engine::OutcomeType;
using exchange::engine::reject_reason;
using exchange::engine::event::command;
using exchange::engine::risk::breach;
using exchange::engine::risk::risk_limits;
using exchange::engine::risk::trading_state;

using exchange::test::risk::buy;
using exchange::test::risk::harness;
using exchange::test::risk::permissive;
using exchange::test::risk::sell;
using exchange::test::risk::SYMBOL;
using exchange::test::risk::TEST_WINDOW_LOG2;
using exchange::test::risk::TEST_WINDOW_NS;

TEST(RiskGateScreening, AnOrderInsideEveryLimitReachesTheSinkUntouched) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	ASSERT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.delivered()[0].as_place().id, 1U);
	EXPECT_TRUE(h.gate().rejections().empty());
	EXPECT_EQ(h.gate().passed(), 1U);
	EXPECT_EQ(h.gate().refused(), 0U);
}

TEST(RiskGateScreening, AcceptingAnOrderCountsItsQuantityAsWorkingExposure) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	EXPECT_EQ(h.working(side_t::bid), 10);
	EXPECT_EQ(h.working(side_t::ask), 0);
	EXPECT_EQ(h.gate().working_orders(), 1U);
	// Nothing has filled, so the position has not moved.
	EXPECT_EQ(h.net(), 0);
}

TEST(RiskGateScreening, AQuantityOverThePerOrderLimitIsRefused) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 100;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 10, 101)));
	EXPECT_TRUE(h.delivered().empty());
	ASSERT_EQ(h.gate().rejections().size(), 1U);
	EXPECT_EQ(h.sole_rejection().type, OutcomeType::REJECTED);
	EXPECT_EQ(h.sole_rejection().id, 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_ORDER_QUANTITY);
	EXPECT_TRUE(h.saw(breach::ORDER_QUANTITY));
	// And a refused order occupies nothing.
	EXPECT_EQ(h.gate().working_orders(), 0U);
	EXPECT_EQ(h.working(side_t::bid), 0);
}

TEST(RiskGateScreening, ExactlyTheLimitIsAdmitted) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 100;
	harness h{limits};
	ASSERT_TRUE(h.place(buy(1, 10, 100)));
	EXPECT_EQ(h.delivered().size(), 1U);
}

TEST(RiskGateScreening, ANotionalOverTheLimitIsRefusedEvenAtAModestSize) {
	// The point of a notional limit: a size that is ordinary on a penny name is
	// a fortune on an expensive one.
	risk_limits limits        = permissive();
	limits.max_order_notional = 10000;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 5000, 3))); // 15,000 tick-lots
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_ORDER_NOTIONAL);
}

TEST(RiskGateScreening, ANonPositiveQuantityIsRefusedBeforeTheBookSeesIt) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 0)));
	EXPECT_TRUE(h.delivered().empty());
	// The book's own reason, because a client must not be able to tell which
	// boundary answered.
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::NON_POSITIVE_QUANTITY);
}

TEST(RiskGateScreening, APriceFarAboveTheMarkIsRefused) {
	risk_limits limits    = permissive();
	limits.price_band_bps = 500; // 5%
	harness h{limits, /*reference=*/1000};
	ASSERT_EQ(h.gate().band_low(), 950U);
	ASSERT_EQ(h.gate().band_high(), 1050U);

	ASSERT_TRUE(h.place(buy(1, 1051, 1)));
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_PRICE_BAND);
}

TEST(RiskGateScreening, APriceFarBelowTheMarkIsRefusedByTheSameCompare) {
	// The floor and the ceiling are one unsigned compare; below the floor the
	// subtraction wraps and fails the same test.
	risk_limits limits    = permissive();
	limits.price_band_bps = 500;
	harness h{limits, 1000};

	ASSERT_TRUE(h.place(sell(1, 949, 1)));
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_PRICE_BAND);
}

TEST(RiskGateScreening, TheBandEdgesThemselvesAreAdmitted) {
	risk_limits limits    = permissive();
	limits.price_band_bps = 500;
	harness h{limits, 1000};

	ASSERT_TRUE(h.place(buy(1, 950, 1)));
	ASSERT_TRUE(h.place(buy(2, 1050, 1)));
	EXPECT_EQ(h.delivered().size(), 2U);
}

TEST(RiskGateScreening, WithNoBandConfiguredEveryPriceIsAdmitted) {
	harness h{permissive(), 1000};
	ASSERT_TRUE(h.place(buy(1, 1, 1)));
	ASSERT_TRUE(h.place(buy(2, 4'000'000'000U, 1)));
	EXPECT_EQ(h.delivered().size(), 2U);
}

TEST(RiskGateScreening, AnOrderThatWouldBreakThePositionLimitIsRefused) {
	risk_limits limits       = permissive();
	limits.max_position_lots = 50;
	harness h{limits};
	h.positions().apply_fill(SYMBOL, side_t::bid, 100, 45);

	ASSERT_TRUE(h.place(buy(1, 100, 6))); // 45 + 6 = 51
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_POSITION_LIMIT);
}

TEST(RiskGateScreening, TheSameOrderOnTheOtherSideReducesRiskAndPasses) {
	// A position limit is on the magnitude, so an order that shrinks the
	// position must not be refused by it however large the position is.
	risk_limits limits       = permissive();
	limits.max_position_lots = 50;
	harness h{limits};
	h.positions().apply_fill(SYMBOL, side_t::bid, 100, 45);

	ASSERT_TRUE(h.place(sell(1, 100, 40)));
	EXPECT_EQ(h.delivered().size(), 1U);
}

TEST(RiskGateScreening, ExposureCountsWorkingOrdersAndNotOnlyFills) {
	// An account holding nothing while showing a thousand orders is one adverse
	// print away from holding all of it. Exposure is valued at the mark.
	risk_limits limits           = permissive();
	limits.max_exposure_notional = 100 * 30; // 30 lots at a mark of 100
	harness h{limits, /*reference=*/100};

	ASSERT_TRUE(h.place(buy(1, 100, 20)));
	ASSERT_EQ(h.delivered().size(), 1U);
	// 20 already working plus 11 more is 31 lots — over the limit, though
	// nothing has filled and the position is still flat.
	ASSERT_TRUE(h.place(buy(2, 100, 11)));
	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_EXPOSURE_LIMIT);
	EXPECT_EQ(h.net(), 0);
}

TEST(RiskGateScreening, ExposureTakesTheWorseSideRatherThanTheSum) {
	// Working on both sides is not twice the exposure: only one of them can be
	// the one that grows the position.
	risk_limits limits           = permissive();
	limits.max_exposure_notional = 100 * 25;
	harness h{limits, 100};

	ASSERT_TRUE(h.place(buy(1, 100, 20)));
	ASSERT_TRUE(h.place(sell(2, 100, 20)));
	// Summing would be 40 and would refuse; the worse side is 20 and does not.
	EXPECT_EQ(h.delivered().size(), 2U);
}

TEST(RiskGateScreening, RunningOutOfMessagesInAWindowRefusesTheNextOrder) {
	risk_limits limits             = permissive();
	limits.max_messages_per_window = 2;
	limits.rate_window_log2_ns     = TEST_WINDOW_LOG2;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 100, 1)));
	ASSERT_TRUE(h.place(buy(2, 100, 1)));
	ASSERT_TRUE(h.place(buy(3, 100, 1)));
	EXPECT_EQ(h.delivered().size(), 2U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_MESSAGE_RATE);
}

TEST(RiskGateScreening, TheNextWindowRestoresTheAllowance) {
	risk_limits limits             = permissive();
	limits.max_messages_per_window = 1;
	limits.rate_window_log2_ns     = TEST_WINDOW_LOG2;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 100, 1)));
	ASSERT_TRUE(h.place(buy(2, 100, 1)));
	ASSERT_EQ(h.delivered().size(), 1U);

	h.clock().advance(TEST_WINDOW_NS);
	ASSERT_TRUE(h.place(buy(3, 100, 1)));
	EXPECT_EQ(h.delivered().size(), 2U);
}

TEST(RiskGateScreening, ARateLimitNeverRefusesACancel) {
	// A throttle that blocks withdrawals is a trap: the moment a strategy most
	// needs to pull its orders is the moment it has been sending the most.
	risk_limits limits             = permissive();
	limits.max_messages_per_window = 1;
	limits.rate_window_log2_ns     = TEST_WINDOW_LOG2;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 100, 1)));
	ASSERT_TRUE(h.cancel(1));
	ASSERT_TRUE(h.cancel(1));
	// The place plus both cancels, though the allowance was one.
	EXPECT_EQ(h.delivered().size(), 3U);
	EXPECT_TRUE(h.gate().rejections().empty());
}

TEST(RiskGateScreening, ATrippedBreakerStopsNewOrdersAndKeepsTheWayOut) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 1)));
	h.breaker().trip(trading_state::CANCEL_ONLY);

	ASSERT_TRUE(h.place(buy(2, 100, 1)));
	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_HALTED);

	ASSERT_TRUE(h.cancel(1));
	EXPECT_EQ(h.delivered().size(), 2U);
}

TEST(RiskGateScreening, AHaltedBreakerStopsTheCancelsToo) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 1)));
	h.breaker().trip(trading_state::HALTED);

	ASSERT_TRUE(h.cancel(1));
	EXPECT_EQ(h.delivered().size(), 1U);
	ASSERT_EQ(h.gate().rejections().size(), 1U);
	EXPECT_EQ(h.sole_rejection().type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_HALTED);
}

TEST(RiskGateScreening, EnoughBreachesInOneWindowTripTheBreakerItself) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 1;
	harness h{limits, /*reference=*/0, /*auto_trip=*/3};

	for (order_id_t id = 1; id <= 3; ++id)
		ASSERT_TRUE(h.place(buy(id, 100, 9)));
	EXPECT_EQ(h.breaker().state(), trading_state::CANCEL_ONLY);

	// Now even a well-sized order is refused, and for the breaker's reason.
	ASSERT_TRUE(h.place(buy(4, 100, 1)));
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_HALTED);
}

TEST(RiskGateScreening, AnIdAlreadyWorkingIsRefusedBeforeItReachesTheQueue) {
	harness h;
	ASSERT_TRUE(h.place(buy(1, 100, 10)));
	ASSERT_TRUE(h.place(buy(1, 200, 5)));

	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::DUPLICATE_ORDER_ID);
	// The first order is untouched.
	EXPECT_EQ(h.working(side_t::bid), 10);
}

TEST(RiskGateScreening, TheLedgerFillingUpIsItsOwnRefusal) {
	risk_limits limits        = permissive();
	limits.max_working_orders = 2;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 100, 1)));
	ASSERT_TRUE(h.place(buy(2, 100, 1)));
	ASSERT_TRUE(h.place(buy(3, 100, 1)));
	EXPECT_EQ(h.delivered().size(), 2U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_WORKING_ORDERS);
}

TEST(RiskGateScreening, BreakingTwoRulesReportsTheSevererAndCountsBoth) {
	risk_limits limits    = permissive();
	limits.max_order_qty  = 1;
	limits.price_band_bps = 100;
	harness h{limits, /*reference=*/1000};

	ASSERT_TRUE(h.place(buy(1, 5000, 999)));
	// PRICE_BAND is bit 4 and ORDER_QUANTITY is bit 2, so the client hears
	// about the size.
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_ORDER_QUANTITY);
	// But the operator can see it was both.
	EXPECT_TRUE(h.saw(breach::ORDER_QUANTITY));
	EXPECT_TRUE(h.saw(breach::PRICE_BAND));
	EXPECT_EQ(h.gate().refused(), 1U);
}

TEST(RiskGateScreening, ARefusedCommandIsDroppedAndTheBatchStillSucceeds) {
	// A risk rejection is not back-pressure: returning false would make a host
	// retry a command that will be refused identically forever.
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	harness h{limits};

	ASSERT_TRUE(h.submit({command::place(buy(1, 100, 1)),
						  command::place(buy(2, 100, 99)),
						  command::place(buy(3, 100, 2))}));

	ASSERT_EQ(h.delivered().size(), 2U);
	EXPECT_EQ(h.delivered()[0].as_place().id, 1U);
	EXPECT_EQ(h.delivered()[1].as_place().id, 3U);
	ASSERT_EQ(h.gate().rejections().size(), 1U);
	EXPECT_EQ(h.sole_rejection().id, 2U);
}

TEST(RiskGateScreening, ABatchWhereEverythingIsRefusedStillReportsSuccess) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 1;
	harness h{limits};

	ASSERT_TRUE(h.submit(
		{command::place(buy(1, 100, 9)), command::place(buy(2, 100, 9))}));
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_EQ(h.gate().rejections().size(), 2U);
	// The sink was never asked, so nothing refused us.
	EXPECT_EQ(h.sink().batches(), 0U);
}

TEST(RiskGateScreening, OrdersInOneBatchAccumulateAgainstTheSameLimit) {
	// Two orders each comfortably inside the position limit, and together over
	// it. Screening the second against the state the first left is the whole
	// reason the batch is walked rather than checked as a set.
	risk_limits limits           = permissive();
	limits.max_position_lots     = 30;
	limits.max_exposure_notional = 100 * 30;
	harness h{limits, /*reference=*/100};

	ASSERT_TRUE(h.submit(
		{command::place(buy(1, 100, 20)), command::place(buy(2, 100, 20))}));
	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.sole_rejection().id, 2U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_EXPOSURE_LIMIT);
}

TEST(RiskGateScreening, RejectionsDescribeOneCallAndDoNotAccumulate) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 1;
	harness h{limits};

	ASSERT_TRUE(h.place(buy(1, 100, 9)));
	ASSERT_EQ(h.gate().rejections().size(), 1U);
	ASSERT_TRUE(h.place(buy(2, 100, 9)));
	EXPECT_EQ(h.gate().rejections().size(), 1U);
	EXPECT_EQ(h.sole_rejection().id, 2U);
	// The lifetime counter is the one that accumulates.
	EXPECT_EQ(h.gate().refused(), 2U);
}

TEST(RiskGateScreening, AnAnonymousAddIsSizeCheckedButProducesNoOutcome) {
	// An ADD rests under the reserved id zero, which the book never reports on,
	// so there is no order for a rejection to name.
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	harness h{limits};

	ASSERT_TRUE(h.submit({command::add(SYMBOL, side_t::bid, 100, 99)}));
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_TRUE(h.gate().rejections().empty());
	EXPECT_TRUE(h.saw(breach::ORDER_QUANTITY));
	EXPECT_EQ(h.gate().refused(), 1U);
}

TEST(RiskGateScreening, AnAnonymousAddInsideTheLimitsPassesWithoutTracking) {
	harness h;
	ASSERT_TRUE(h.submit({command::add(SYMBOL, side_t::bid, 100, 5)}));
	EXPECT_EQ(h.delivered().size(), 1U);
	// Deliberately untracked: nothing will ever retire it. @see screen_add
	EXPECT_EQ(h.gate().working_orders(), 0U);
	EXPECT_EQ(h.working(side_t::bid), 0);
}

TEST(RiskGateScreening, AnEmptyBatchIsAccepted) {
	harness h;
	EXPECT_TRUE(h.submit({}));
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_EQ(h.sink().batches(), 0U);
}

} // namespace
