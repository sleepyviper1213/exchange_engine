// What happens when the *sink* says no.
//
// This is the suite that pins the property the whole screen-deliver-commit
// ordering exists for: a batch the gateway could not take must leave the gate
// exactly as it found it, because the host is about to hand the identical batch
// back. Without the rollback, the second attempt would refuse every order it
// accepted the first time with DUPLICATE_ORDER_ID and the strategy would be
// wedged for the rest of the session.

#include "gate.fixture.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book/reject_reason.hpp"
#include "trading-engine/orders/types.hpp"

#include <gtest/gtest.h>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::risk;

TEST(RiskGateBackpressure, ARefusedDeliveryIsReportedAsBackPressure) {
	harness h;
	h.sink().refuse(true);

	EXPECT_FALSE(h.place(buy(1, 100, 10)));
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_EQ(h.gate().stalls(), 1U);
}

TEST(RiskGateBackpressure, ARefusedDeliveryLeavesTheLedgerAsItFoundIt) {
	harness h;
	h.sink().refuse(true);
	ASSERT_FALSE(h.place(buy(1, 100, 10)));

	EXPECT_EQ(h.gate().working_orders(), 0U);
	EXPECT_FALSE(h.gate().ledger().contains(1));
	EXPECT_EQ(h.working(side_t::bid), 0);
}

TEST(RiskGateBackpressure, TheIdenticalBatchSucceedsOnceTheSinkRecovers) {
	// The headline property. A host retries with the same commands; the gate
	// must behave as though the refused attempt never happened.
	harness h;
	h.sink().refuse(true);
	ASSERT_FALSE(h.place(buy(1, 100, 10)));

	h.sink().refuse(false);
	ASSERT_TRUE(h.place(buy(1, 100, 10)));

	ASSERT_EQ(h.delivered().size(), 1U);
	EXPECT_EQ(h.delivered()[0].as_place().id, 1U);
	EXPECT_TRUE(h.gate().rejections().empty());
	EXPECT_EQ(h.gate().working_orders(), 1U);
	EXPECT_EQ(h.working(side_t::bid), 10);
	EXPECT_EQ(h.gate().passed(), 1U);
}

TEST(RiskGateBackpressure, RetryingAWholeBatchDoesNotDoubleCountExposure) {
	harness h;
	h.sink().refuse(true);
	ASSERT_FALSE(h.submit(
		{command::place(buy(1, 100, 10)), command::place(buy(2, 100, 10))}));
	ASSERT_EQ(h.working(side_t::bid), 0);

	h.sink().refuse(false);
	ASSERT_TRUE(h.submit(
		{command::place(buy(1, 100, 10)), command::place(buy(2, 100, 10))}));
	EXPECT_EQ(h.working(side_t::bid), 20);
	EXPECT_EQ(h.gate().working_orders(), 2U);
}

TEST(RiskGateBackpressure, ARefusedDeliveryDoesNotSpendTheRateWindow) {
	// If it did, a stalling consumer would throttle the producer for reasons
	// that have nothing to do with its message rate.
	risk_limits limits             = permissive();
	limits.max_messages_per_window = 1;
	limits.rate_window_log2_ns     = TEST_WINDOW_LOG2;
	harness h{limits};

	h.sink().refuse(true);
	ASSERT_FALSE(h.place(buy(1, 100, 1)));

	h.sink().refuse(false);
	ASSERT_TRUE(h.place(buy(1, 100, 1)));
	EXPECT_EQ(h.delivered().size(), 1U);
	EXPECT_TRUE(h.gate().rejections().empty());
}

TEST(RiskGateBackpressure, ARefusedDeliveryReportsNothingToTheClient) {
	// Nothing happened, so nobody is told anything. Reporting here would send a
	// client a rejection for an order that is about to be submitted again.
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	harness h{limits};
	h.sink().refuse(true);

	ASSERT_FALSE(h.submit(
		{command::place(buy(1, 100, 1)), command::place(buy(2, 100, 99))}));
	EXPECT_TRUE(h.gate().rejections().empty());
	EXPECT_EQ(h.gate().refused(), 0U);
}

TEST(RiskGateBackpressure, ARefusedDeliveryDoesNotCountTowardsTheBreaker) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	harness h{limits, /*reference=*/0, auto_trip_after{2}};
	h.sink().refuse(true);

	// Two oversized orders, twice — four breaches' worth if they counted.
	for (int attempt = 0; attempt < 2; ++attempt)
		ASSERT_FALSE(h.submit(
			{command::place(buy(1, 100, 1)), command::place(buy(2, 100, 99))}));
	EXPECT_EQ(h.breaker().breaches(0), 0U);
	EXPECT_TRUE(h.breaker().passes_new_orders());
}

TEST(RiskGateBackpressure, ABatchThatIsEntirelyRefusedNeverAsksTheSink) {
	// Nothing survives screening, so there is nothing to be back-pressured on —
	// and a sink that is refusing must not turn that into a stall.
	risk_limits limits   = permissive();
	limits.max_order_qty = 1;
	harness h{limits};
	h.sink().refuse(true);

	EXPECT_TRUE(h.place(buy(1, 100, 9)));
	EXPECT_EQ(h.gate().stalls(), 0U);
	EXPECT_EQ(h.sink().refusals(), 0U);
	EXPECT_EQ(h.gate().rejections().size(), 1U);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_ORDER_QUANTITY);
}

TEST(RiskGateBackpressure, OnlyTheSurvivorsAreOfferedToTheSink) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	harness h{limits};

	ASSERT_TRUE(h.submit({command::place(buy(1, 100, 1)),
						  command::place(buy(2, 100, 99)),
						  command::place(buy(3, 100, 1))}));
	// One batch, two commands — the refused one never occupied a queue slot.
	EXPECT_EQ(h.sink().batches(), 1U);
	EXPECT_EQ(h.delivered().size(), 2U);
}

TEST(RiskGateBackpressure, RepeatedStallsAccumulateOnTheCounter) {
	harness h;
	h.sink().refuse(true);
	for (int i = 0; i < 3; ++i) ASSERT_FALSE(h.place(buy(1, 100, 1)));
	EXPECT_EQ(h.gate().stalls(), 3U);
	EXPECT_EQ(h.gate().passed(), 0U);
}

} // namespace
