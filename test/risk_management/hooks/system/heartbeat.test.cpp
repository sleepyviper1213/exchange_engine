// The watchdog: what silence from the venue does, and what it deliberately does
// not do.
//
// Every claim here is about time, and time arrives as an argument - so the suite
// names the readings it wants instead of moving a clock and hoping. That is the
// same reason `rate_limiter`'s suite can step across a window in a literal.

#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management/hooks/system/heartbeat.hpp"
#include "risk_management/hooks/system/trading_state.hpp"

#include <gtest/gtest.h>

#include <cstdint>


namespace {

using namespace exchange::risk;
using exchange::risk::hooks::system::heartbeat_monitor;

/// @brief Silence a test can step over in one literal.
constexpr std::uint64_t TIMEOUT_NS = 1'000;

/// @brief The reading a monitor is born at, chosen non-zero so a test cannot
///        pass by accident on an uninitialised zero.
constexpr std::uint64_t BIRTH_NS = 10'000;

TEST(RiskHooksHeartbeat, ConstructionCountsAsABeat) {
	// Otherwise a monitor built before the feed connects trips on its first poll,
	// for silence that predates it.
	circuit_breaker breaker;
	const heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};

	EXPECT_EQ(monitor.last_beat_ns(), BIRTH_NS);
	EXPECT_EQ(monitor.silence_ns(BIRTH_NS), 0U);
	EXPECT_FALSE(monitor.is_silent(BIRTH_NS));
	EXPECT_EQ(monitor.beats(), 0U);
}

TEST(RiskHooksHeartbeat, SilenceInsideTheTimeoutIsNotATrip) {
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};

	// Exactly the timeout is still inside it - the last admissible value, not the
	// first refused one, the way the loss floor reads.
	EXPECT_EQ(monitor.silence_ns(BIRTH_NS + TIMEOUT_NS), TIMEOUT_NS);
	EXPECT_FALSE(monitor.is_silent(BIRTH_NS + TIMEOUT_NS));
	EXPECT_FALSE(monitor.poll(BIRTH_NS + TIMEOUT_NS));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
}

TEST(RiskHooksHeartbeat, SilencePastTheTimeoutTripsToCancelOnly) {
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};

	EXPECT_TRUE(monitor.is_silent(BIRTH_NS + TIMEOUT_NS + 1));
	EXPECT_TRUE(monitor.poll(BIRTH_NS + TIMEOUT_NS + 1));

	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(breaker.cause(), trip_cause::STALE_FEED);
	EXPECT_EQ(monitor.trips(), 1U);
	// The half that matters in the emergency: a strategy that has lost its market
	// data must still be able to pull the quotes it has already shown.
	EXPECT_FALSE(breaker.passes_new_orders());
	EXPECT_TRUE(breaker.passes_cancels());
}

TEST(RiskHooksHeartbeat, ABeatResetsTheSilence) {
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};

	monitor.beat(BIRTH_NS + TIMEOUT_NS);
	EXPECT_EQ(monitor.beats(), 1U);
	EXPECT_EQ(monitor.silence_ns(BIRTH_NS + TIMEOUT_NS), 0U);
	EXPECT_FALSE(monitor.poll(BIRTH_NS + 2 * TIMEOUT_NS));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
}

TEST(RiskHooksHeartbeat, AWatchdogNobodyPollsNeverTrips) {
	// Stated as a test because it is the failure mode of the design: nothing here
	// runs on a timer, so a run loop that forgets to poll has a monitor that
	// reports staleness to nobody.
	circuit_breaker breaker;
	const heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};

	EXPECT_TRUE(monitor.is_silent(BIRTH_NS + TIMEOUT_NS * 1'000));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_EQ(monitor.trips(), 0U);
}

TEST(RiskHooksHeartbeat, OneOutageIsOneTrip) {
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};
	ASSERT_TRUE(monitor.poll(BIRTH_NS + TIMEOUT_NS + 1));

	// Still silent, still polled, and it does not re-trip a breaker that is
	// already open - an operator counting trips wants outages, not polls.
	EXPECT_FALSE(monitor.poll(BIRTH_NS + TIMEOUT_NS * 10));
	EXPECT_EQ(monitor.trips(), 1U);
	EXPECT_EQ(breaker.trips(), 1U);
}

TEST(RiskHooksHeartbeat, ARearmIntoAStillSilentFeedTripsAgain) {
	// Deliberate, and the same contract circuit_breaker documents for its own
	// re-arm: a condition that still holds does not buy a fresh allowance.
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};
	ASSERT_TRUE(monitor.poll(BIRTH_NS + TIMEOUT_NS + 1));

	breaker.arm();
	ASSERT_TRUE(breaker.passes_new_orders());

	EXPECT_TRUE(monitor.poll(BIRTH_NS + TIMEOUT_NS + 2));
	EXPECT_EQ(monitor.trips(), 2U);
	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
}

TEST(RiskHooksHeartbeat, ARecoveredFeedDoesNotReArmTheBreakerByItself) {
	// The mirror of the loss floor: the feed coming back means the link healed,
	// not that anybody decided to keep trading.
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};
	ASSERT_TRUE(monitor.poll(BIRTH_NS + TIMEOUT_NS + 1));

	monitor.beat(BIRTH_NS + TIMEOUT_NS + 2);
	EXPECT_FALSE(monitor.is_silent(BIRTH_NS + TIMEOUT_NS + 2));
	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(breaker.cause(), trip_cause::STALE_FEED);
}

TEST(RiskHooksHeartbeat, NoTimeoutNeverTrips) {
	// The disabled configuration, spelled rather than achieved by leaving the
	// monitor out of the wiring.
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, heartbeat_monitor::NO_TIMEOUT, BIRTH_NS};

	EXPECT_FALSE(monitor.is_silent(BIRTH_NS + TIMEOUT_NS * 1'000'000));
	EXPECT_FALSE(monitor.poll(BIRTH_NS + TIMEOUT_NS * 1'000'000));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_EQ(monitor.timeout_ns(), heartbeat_monitor::NO_TIMEOUT);
}

TEST(RiskHooksHeartbeat, TheCauseIsWhatTellsAnOperatorWhereToLook) {
	// A STALE_FEED trip is the one whose fix is not in this process, which is why
	// it is a cause of its own rather than folded into the operator trip.
	circuit_breaker breaker;
	heartbeat_monitor monitor{breaker, TIMEOUT_NS, BIRTH_NS};
	ASSERT_TRUE(monitor.poll(BIRTH_NS + TIMEOUT_NS + 1));

	EXPECT_EQ(breaker.cause(), trip_cause::STALE_FEED);
	EXPECT_NE(breaker.cause(), trip_cause::OPERATOR);
	EXPECT_NE(breaker.cause(), trip_cause::LOSS_LIMIT);
}

} // namespace
