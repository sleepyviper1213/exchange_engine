// The loss floor: the one rule that stops trading rather than refusing an
// order.
//
// Everything else the gate does is a per-command decision. This is not - a
// losing position is not the fault of the order in front of you, so refusing
// that one order while accepting the next identical one would be incoherent.
// The floor trips the breaker instead, and a human has to undo it.

#include "gate.fixture.hpp"
#include "trading-engine/order_book/reject_reason.hpp"
#include "risk_management/circuit_breaker.hpp"

#include <gtest/gtest.h>


namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::risk;

/// @brief Limits that stop trading once 500 tick-lots have been lost.
[[nodiscard]] risk_limits with_floor(std::int64_t loss = 500) {
	risk_limits limits = permissive();
	limits.max_loss    = loss;
	return limits;
}

/// @brief Buy @p qty at @p price through the gate and have it fill there, so
///        the harness ends up holding a real position.
void go_long(harness &h, order_id_t id, price_t price, quantity_t qty) {
	ASSERT_TRUE(h.place(buy(id, price, qty)));
	h.filled(id, /*resting=*/0, price, qty);
}

TEST(RiskGateLossLimit, AProfitableAccountIsLeftAlone) {
	harness h{with_floor(), /*reference=*/100};
	go_long(h, 1, 100, 10);

	// Marked up: +100.
	h.filled(900, 901, 110, 1);
	EXPECT_EQ(h.gate().pnl(), 100);
	EXPECT_TRUE(h.breaker().passes_new_orders());
	EXPECT_EQ(h.breaker().cause(), trip_cause::NONE);
}

TEST(RiskGateLossLimit, ALossInsideTheFloorDoesNotTrip) {
	harness h{with_floor(500), 100};
	go_long(h, 1, 100, 10);

	// Down 10 ticks on 10 lots is -100, well inside a 500 floor.
	h.filled(900, 901, 90, 1);
	EXPECT_EQ(h.gate().pnl(), -100);
	EXPECT_TRUE(h.breaker().passes_new_orders());
}

TEST(RiskGateLossLimit, ExactlyTheFloorIsStillInsideIt) {
	harness h{with_floor(500), 100};
	go_long(h, 1, 100, 10);

	h.filled(900, 901, 50, 1); // -500 exactly
	EXPECT_EQ(h.gate().pnl(), -500);
	EXPECT_TRUE(h.breaker().passes_new_orders());
}

TEST(RiskGateLossLimit, FallingThroughTheFloorTripsTheBreaker) {
	harness h{with_floor(500), 100};
	go_long(h, 1, 100, 10);

	h.filled(900, 901, 49, 1); // -510
	EXPECT_EQ(h.gate().pnl(), -510);
	EXPECT_EQ(h.breaker().state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(h.breaker().cause(), trip_cause::LOSS_LIMIT);
	EXPECT_EQ(h.breaker().trips(), 1U);
}

TEST(RiskGateLossLimit, TheMarketMovingAgainstUsIsEnoughOnItsOwn) {
	// The half a fill-driven check would miss: this account does nothing at
	// all, and somebody else's print is what puts it through the floor.
	harness h{with_floor(500), 100};
	go_long(h, 1, 100, 10);
	ASSERT_TRUE(h.breaker().passes_new_orders());

	h.filled(/*aggressor=*/900, /*resting=*/901, /*price=*/20, /*volume=*/1);
	EXPECT_EQ(h.breaker().state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(h.breaker().cause(), trip_cause::LOSS_LIMIT);
}

TEST(RiskGateLossLimit, AShortIsCaughtByTheMarketRisingInstead) {
	harness h{with_floor(500), 100};
	ASSERT_TRUE(h.place(sell(1, 100, 10)));
	h.filled(1, 0, 100, 10);
	ASSERT_EQ(h.net(), -10);

	h.filled(900, 901, 160, 1); // short 10 from 100, now 160: -600
	EXPECT_EQ(h.gate().pnl(), -600);
	EXPECT_EQ(h.breaker().cause(), trip_cause::LOSS_LIMIT);
}

TEST(RiskGateLossLimit, ATrippedGateRefusesNewOrdersAndStillTakesCancels) {
	// CANCEL_ONLY, not HALTED: the whole point is to shed the position that
	// caused the loss.
	harness h{with_floor(500), 100};
	go_long(h, 1, 100, 10);
	ASSERT_TRUE(h.place(buy(2, 100, 1)));
	h.filled(900, 901, 40, 1);
	ASSERT_EQ(h.breaker().state(), trading_state::CANCEL_ONLY);

	const std::size_t before = h.delivered().size();
	ASSERT_TRUE(h.place(buy(3, 40, 1)));
	EXPECT_EQ(h.delivered().size(), before);
	EXPECT_EQ(h.sole_rejection().reason, reject_reason::RISK_HALTED);

	ASSERT_TRUE(h.cancel(2));
	EXPECT_EQ(h.delivered().size(), before + 1);
}

TEST(RiskGateLossLimit, BleedingFurtherDoesNotTripAgain) {
	// One trip and one cause, however many prints the drawdown takes - an
	// operator counting trips wants events, not ticks.
	harness h{with_floor(500), 100};
	go_long(h, 1, 100, 10);

	for (price_t mark = 45; mark >= 10; mark -= 5) h.filled(900, 901, mark, 1);
	EXPECT_EQ(h.breaker().trips(), 1U);
}

TEST(RiskGateLossLimit, RecoveringDoesNotReArmTheBreakerByItself) {
	// Deliberate: coming back above the floor means the position moved, not
	// that anybody decided to keep trading. Re-arming is a human's call.
	harness h{with_floor(500), 100};
	go_long(h, 1, 100, 10);
	h.filled(900, 901, 40, 1);
	ASSERT_EQ(h.breaker().state(), trading_state::CANCEL_ONLY);

	h.filled(900, 901, 200, 1);
	ASSERT_GT(h.gate().pnl(), 0);
	EXPECT_EQ(h.breaker().state(), trading_state::CANCEL_ONLY);

	h.breaker().arm();
	EXPECT_TRUE(h.breaker().passes_new_orders());
	// The cause survives the re-arm; it is history, not current state.
	EXPECT_EQ(h.breaker().cause(), trip_cause::LOSS_LIMIT);
}

TEST(RiskGateLossLimit, NoFloorConfiguredMeansNoTripHoweverBadItGets) {
	harness h{permissive(), 100};
	go_long(h, 1, 100, 10);
	h.filled(900, 901, 1, 1);

	EXPECT_LT(h.gate().pnl(), -900);
	EXPECT_TRUE(h.breaker().passes_new_orders());
	EXPECT_EQ(h.breaker().cause(), trip_cause::NONE);
}

TEST(RiskGateLossLimit, TheTwoAutomaticTripsAreToldApart) {
	// A looping strategy and a losing one need different responses, so the
	// cause has to distinguish them.
	risk_limits limits   = with_floor(500);
	limits.max_order_qty = 1;
	harness h{limits, /*reference=*/100, auto_trip_after{2}};

	ASSERT_TRUE(h.place(buy(1, 100, 99)));
	ASSERT_TRUE(h.place(buy(2, 100, 99)));
	EXPECT_EQ(h.breaker().state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(h.breaker().cause(), trip_cause::BREACH_RATE);
}

} // namespace
