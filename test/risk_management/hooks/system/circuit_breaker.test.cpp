// The kill switch: what each state lets through, and what it takes to trip it.

#include "risk_management/hooks/system/circuit_breaker.hpp"

// at_ns - the one place an integer is turned into a monotonic reading. The
// breaker's window rules are stated as numbers either side of an edge, and
// spelling the conversion is what makes the integer visibly an instant. @see
// risk.fixture.hpp
#include "../../risk.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using exchange::risk::hooks::system::circuit_breaker;
using exchange::risk::hooks::system::trading_state;

constexpr unsigned BREAKER_SMALL_WINDOW_LOG2 = 10;
constexpr std::uint64_t BREAKER_WINDOW_NS = 1ull << BREAKER_SMALL_WINDOW_LOG2;

TEST(RiskCircuitBreaker, ABreakerStartsClosedAndPassesEverything) {
	const circuit_breaker breaker;
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_TRUE(breaker.passes_new_orders());
	EXPECT_TRUE(breaker.passes_cancels());
	EXPECT_EQ(breaker.trips(), 0ull);
}

TEST(RiskCircuitBreaker, CancelOnlyStopsNewLiquidityAndKeepsTheWayOut) {
	// The whole reason CANCEL_ONLY exists: a stop that also froze the
	// withdrawals would leave a malfunctioning strategy's orders resting in a
	// book nobody is managing.
	circuit_breaker breaker;
	breaker.trip(trading_state::CANCEL_ONLY);
	EXPECT_FALSE(breaker.passes_new_orders());
	EXPECT_TRUE(breaker.passes_cancels());
	EXPECT_EQ(breaker.trips(), 1ull);
}

TEST(RiskCircuitBreaker, HaltedStopsTheCancelsToo) {
	circuit_breaker breaker;
	breaker.trip(trading_state::HALTED);
	EXPECT_FALSE(breaker.passes_new_orders());
	EXPECT_FALSE(breaker.passes_cancels());
}

TEST(RiskCircuitBreaker, ArmingReopensTheBreaker) {
	circuit_breaker breaker;
	breaker.trip(trading_state::HALTED);
	breaker.arm();
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_TRUE(breaker.passes_new_orders());
	// The trip is still on the record; re-arming is not forgetting.
	EXPECT_EQ(breaker.trips(), 1ull);
}

TEST(RiskCircuitBreaker, ABreakerWithNoThresholdNeverTripsItself) {
	circuit_breaker breaker{circuit_breaker::NO_AUTO_TRIP,
							BREAKER_SMALL_WINDOW_LOG2};
	for (int i = 0; i < 1000; ++i)
		EXPECT_FALSE(breaker.record_breach(at_ns(0)));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
}

TEST(RiskCircuitBreaker, ReachingTheThresholdInOneWindowTripsToCancelOnly) {
	circuit_breaker breaker{3, BREAKER_SMALL_WINDOW_LOG2};
	EXPECT_FALSE(breaker.record_breach(at_ns(0)));
	EXPECT_FALSE(breaker.record_breach(at_ns(1)));
	EXPECT_TRUE(breaker.record_breach(at_ns(2)));
	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
	EXPECT_TRUE(breaker.passes_cancels());
}

TEST(RiskCircuitBreaker, OnlyTheCallThatTripsItReportsTrue) {
	circuit_breaker breaker{2, BREAKER_SMALL_WINDOW_LOG2};
	ASSERT_FALSE(breaker.record_breach(at_ns(0)));
	ASSERT_TRUE(breaker.record_breach(at_ns(0)));
	// Already open - later breaches are counted but do not re-trip.
	EXPECT_FALSE(breaker.record_breach(at_ns(0)));
	EXPECT_EQ(breaker.trips(), 1ull);
}

TEST(RiskCircuitBreaker, BreachesInDifferentWindowsDoNotAccumulate) {
	// This is the whole point of the window: a strategy breaching once every
	// few milliseconds is sizing against a moving position, not looping.
	circuit_breaker breaker{3, BREAKER_SMALL_WINDOW_LOG2};
	EXPECT_FALSE(breaker.record_breach(at_ns(0)));
	EXPECT_FALSE(breaker.record_breach(at_ns(BREAKER_WINDOW_NS)));
	EXPECT_FALSE(breaker.record_breach(at_ns(BREAKER_WINDOW_NS * 2)));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_EQ(breaker.breaches(at_ns(BREAKER_WINDOW_NS * 2)), 1ull);
}

TEST(RiskCircuitBreaker, TheCounterReportsOnlyTheCurrentWindow) {
	circuit_breaker breaker{100, BREAKER_SMALL_WINDOW_LOG2};
	breaker.record_breach(at_ns(0));
	breaker.record_breach(at_ns(1));
	EXPECT_EQ(breaker.breaches(at_ns(2)), 2U);
	EXPECT_EQ(breaker.breaches(at_ns(BREAKER_WINDOW_NS)), 0U);
}

TEST(RiskCircuitBreaker, ReArmingDoesNotGrantAFreshAllowance) {
	// A re-arm into a still-looping strategy should trip again on the next
	// breach rather than give it three more.
	circuit_breaker breaker{2, BREAKER_SMALL_WINDOW_LOG2};
	breaker.record_breach(at_ns(0));
	ASSERT_TRUE(breaker.record_breach(at_ns(0)));
	breaker.arm();
	ASSERT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_TRUE(breaker.record_breach(at_ns(0)));
	EXPECT_EQ(breaker.trips(), 2U);
}

TEST(RiskCircuitBreaker, EveryStateHasAName) {
	EXPECT_EQ(to_string(trading_state::NORMAL), "NORMAL");
	EXPECT_EQ(to_string(trading_state::CANCEL_ONLY), "CANCEL_ONLY");
	EXPECT_EQ(to_string(trading_state::HALTED), "HALTED");
	EXPECT_FALSE(describe(trading_state::CANCEL_ONLY).empty());
}

} // namespace
