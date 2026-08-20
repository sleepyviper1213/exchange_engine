// Being filled too fast, and being filled all the way down.
//
// The two halves are deliberately asserted apart, because they fail apart: a
// burst is a windowed count and forgets, a run is evidence and does not. The
// assertions that matter most are the ones about what does *not* extend a run -
// a flat print, and the first print of the session - because a run counter that
// is generous about those fires on a quiet market and gets switched off.

#include "post_trade.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::risk;
using namespace exchange::risk::hooks::post_trade;

// --- the arithmetic, at compile time -------------------------------------

static_assert(!is_over_burst(1000, 1'000'000, 0, 0),
			  "both halves disabled is a rule that does nothing");

static_assert(is_over_burst(4, 0, 3, 0), "the count half stands alone");
static_assert(is_over_burst(0, 40, 0, 30), "and so does the volume half");
static_assert(!is_over_burst(3, 30, 3, 30),
			  "exactly the cap is admissible, on either half");

static_assert(!is_over_run(1000, 0), "a run cap of zero disables it");
static_assert(!is_over_run(3, 3), "the cap is the last admissible run");
static_assert(is_over_run(4, 3), "one more is not");

// --- the burst half ------------------------------------------------------

TEST(PostTradeFillBurst, TripsOnThePrintPastTheCount) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(3, 0, fill_burst::NO_LIMIT)};

	for (int i = 0; i < 3; ++i) EXPECT_FALSE(rule.record(0, print(100, 1)));
	EXPECT_FALSE(rule.is_bursting(0));

	EXPECT_TRUE(rule.record(0, print(100, 1)));
	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(breaker.cause(), trip_cause::FILL_BURST);
	EXPECT_EQ(rule.executions(0), 4U);
}

TEST(PostTradeFillBurst, TripsOnVolumeWithTheCountDisabled) {
	circuit_breaker breaker;
	fill_burst rule{
		breaker,
		surveillance_burst(fill_burst::NO_LIMIT, 10, fill_burst::NO_LIMIT)};

	EXPECT_FALSE(rule.record(0, print(100, 4)));
	EXPECT_FALSE(rule.record(0, print(100, 6)))
		<< "ten lots is exactly the cap";
	EXPECT_EQ(rule.volume(0), 10U);

	EXPECT_TRUE(rule.record(0, print(100, 1)));
	EXPECT_EQ(breaker.cause(), trip_cause::FILL_BURST)
		<< "one thousand-lot print is as much of a burst as a thousand prints";
}

TEST(PostTradeFillBurst, ANewWindowForgetsTheBurst) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(2, 0, fill_burst::NO_LIMIT)};

	EXPECT_FALSE(rule.record(0, print(100, 1)));
	EXPECT_FALSE(rule.record(0, print(100, 1)));
	EXPECT_EQ(rule.executions(0), 2U);

	EXPECT_FALSE(rule.record(POST_TRADE_WINDOW_NS, print(100, 1)))
		<< "the third print, but the first of its window";
	EXPECT_EQ(rule.executions(POST_TRADE_WINDOW_NS), 1U);
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
}

// --- the run half --------------------------------------------------------

TEST(PostTradeFillBurst, TheFirstPrintEstablishesAPriceAndNotADirection) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(fill_burst::NO_LIMIT, 0, 1)};

	EXPECT_FALSE(rule.record(0, print(100, 1)));
	EXPECT_EQ(rule.last_price(), 100U);
	EXPECT_EQ(rule.direction(), tape_direction::UNKNOWN)
		<< "one price is not a move";
	EXPECT_EQ(rule.run(), 0U);
}

TEST(PostTradeFillBurst, TripsWhenTheTapeRunsPastTheCap) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(fill_burst::NO_LIMIT, 0, 3)};

	rule.record(0, print(100, 1));
	for (price_t price = 101; price <= 103; ++price)
		EXPECT_FALSE(rule.record(0, print(price, 1)));

	EXPECT_EQ(rule.direction(), tape_direction::UP);
	EXPECT_EQ(rule.run(), 3U);
	EXPECT_FALSE(rule.is_running()) << "three is the cap, so three is allowed";

	EXPECT_TRUE(rule.record(0, print(104, 1)));
	EXPECT_EQ(breaker.cause(), trip_cause::ADVERSE_RUN);
	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
}

TEST(PostTradeFillBurst, AFlatPrintNeitherExtendsNorBreaksARun) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(fill_burst::NO_LIMIT, 0, 10)};

	rule.record(0, print(100, 1));
	rule.record(0, print(101, 1));
	EXPECT_EQ(rule.run(), 1U);

	rule.record(0, print(101, 1));
	EXPECT_EQ(rule.run(), 1U) << "nothing moved, so nothing is said";
	EXPECT_EQ(rule.direction(), tape_direction::UP);

	rule.record(0, print(102, 1));
	EXPECT_EQ(rule.run(), 2U) << "and the run it did not break carries on";
}

TEST(PostTradeFillBurst, AReversalStartsTheRunOver) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(fill_burst::NO_LIMIT, 0, 10)};

	for (price_t price = 100; price <= 104; ++price)
		rule.record(0, print(price, 1));
	EXPECT_EQ(rule.run(), 4U);
	EXPECT_EQ(rule.direction(), tape_direction::UP);

	rule.record(0, print(103, 1));
	EXPECT_EQ(rule.direction(), tape_direction::DOWN);
	EXPECT_EQ(rule.run(), 1U) << "the reversal is itself one move down";
}

TEST(PostTradeFillBurst, ARunOutlivesTheWindow) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(fill_burst::NO_LIMIT, 0, 3)};

	std::uint64_t now = 0;
	rule.record(now, print(100, 1));
	for (price_t price = 101; price <= 103; ++price) {
		now += POST_TRADE_WINDOW_NS;
		EXPECT_FALSE(rule.record(now, print(price, 1)));
	}

	EXPECT_EQ(rule.executions(now), 1U) << "each print is alone in its window";
	EXPECT_EQ(rule.run(), 3U) << "and the run remembers all of them";

	now += POST_TRADE_WINDOW_NS;
	EXPECT_TRUE(rule.record(now, print(104, 1)))
		<< "a slow run is still a run - a window would have thrown away the "
		   "version that is hardest to notice";
}

// --- the two together ----------------------------------------------------

TEST(PostTradeFillBurst, BurstIsDiagnosedBeforeRun) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(2, 0, 1)};

	rule.record(0, print(100, 1));
	rule.record(0, print(101, 1));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);

	// This print is over the count cap *and* over the run cap.
	EXPECT_TRUE(rule.record(0, print(102, 1)));
	EXPECT_TRUE(rule.is_running()) << "the run is over its cap too";
	EXPECT_EQ(breaker.cause(), trip_cause::FILL_BURST)
		<< "but too much at once is the reading to look at first";
	EXPECT_EQ(rule.trips(), 1U) << "and one print produces one trip";
}

TEST(PostTradeFillBurst, KeepsCountingThroughAnOpenBreaker) {
	circuit_breaker breaker;
	fill_burst rule{breaker, surveillance_burst(1, 0, fill_burst::NO_LIMIT)};

	rule.record(0, print(100, 5));
	EXPECT_TRUE(rule.record(0, print(101, 5)));
	EXPECT_FALSE(rule.record(0, print(102, 5)))
		<< "already tripped, so not again";

	EXPECT_EQ(rule.total_executions(), 3U);
	EXPECT_EQ(rule.total_volume(), 15U)
		<< "an operator re-arming must not be re-arming into numbers that "
		   "stopped when they got interesting";
	EXPECT_EQ(rule.trips(), 1U);
}

TEST(PostTradeFillBurst, ADisabledRuleNeverTrips) {
	circuit_breaker breaker;
	fill_burst rule{
		breaker,
		surveillance_burst(fill_burst::NO_LIMIT, 0, fill_burst::NO_LIMIT)};

	for (price_t price = 100; price < 200; ++price)
		rule.record(0, print(price, 100));

	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_EQ(rule.trips(), 0U);
	EXPECT_EQ(rule.total_executions(), 100U);
	EXPECT_EQ(rule.run(), 99U) << "it still knows; it just does not act";
}

} // namespace
