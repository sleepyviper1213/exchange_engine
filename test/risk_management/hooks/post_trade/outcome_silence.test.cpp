// Exposure the process believes in and has heard nothing about.
//
// The rule is a conjunction, and both halves of it are load-bearing in
// opposite directions - so the two assertions that matter most are the ones
// where it must *not* fire. A strategy that has gone flat and idle is silent
// for hours and is not broken; a busy strategy hearing back constantly has
// orders working and is not broken either. Only the pair is a fault, and a
// watchdog that gets that wrong is a watchdog somebody switches off.

#include "post_trade.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::risk;
using namespace exchange::risk::hooks::post_trade;

// --- the arithmetic, at compile time -------------------------------------

static_assert(!is_silent_with_exposure(1'000'000'000, 10, 0),
			  "a timeout of zero disables the rule");

static_assert(!is_silent_with_exposure(1'000'000'000, 0, 1000),
			  "silence with nothing working is an idle strategy, not a fault");

static_assert(!is_silent_with_exposure(0, 10, 1000),
			  "orders working and the path answering is the ordinary case");

static_assert(!is_silent_with_exposure(1000, 10, 1000),
			  "exactly the timeout is the last admissible silence");

static_assert(is_silent_with_exposure(1001, 10, 1000),
			  "one nanosecond more is the first refused one");

// --- what the rule does --------------------------------------------------

TEST(PostTradeOutcomeSilence, TripsOnSilenceWithExposure) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(POST_TRADE_TIMEOUT_NS),
						 at_ns(0)};

	EXPECT_FALSE(rule.poll(at_ns(POST_TRADE_TIMEOUT_NS), 1))
		<< "exactly the timeout is still admissible";
	EXPECT_TRUE(rule.poll(at_ns(POST_TRADE_TIMEOUT_NS + 1), 1));

	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY)
		<< "a strategy that cannot hear the venue must keep the ability to "
		   "pull its quotes";
	EXPECT_EQ(breaker.cause(), trip_cause::STALE_WORKING);
	EXPECT_EQ(rule.trips(), 1U);
}

TEST(PostTradeOutcomeSilence, IdleAndSilentIsNotAFault) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(POST_TRADE_TIMEOUT_NS),
						 at_ns(0)};

	EXPECT_FALSE(rule.poll(at_ns(POST_TRADE_TIMEOUT_NS * 1000), 0));
	EXPECT_FALSE(rule.is_silent(at_ns(POST_TRADE_TIMEOUT_NS * 1000), 0));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL)
		<< "a strategy with nothing working has nothing to be told about";
}

TEST(PostTradeOutcomeSilence, AnOutcomeIsEvidenceThePathIsAlive) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(POST_TRADE_TIMEOUT_NS),
						 at_ns(0)};

	const std::uint64_t nearly = POST_TRADE_TIMEOUT_NS;
	EXPECT_FALSE(rule.poll(at_ns(nearly), 1));

	rule.beat(at_ns(nearly));
	EXPECT_EQ(rule.last_outcome(), at_ns(nearly));
	EXPECT_EQ(rule.silence_ns(at_ns(nearly)), 0U);
	EXPECT_EQ(rule.outcomes(), 1U);

	EXPECT_FALSE(rule.poll(at_ns(nearly + POST_TRADE_TIMEOUT_NS), 1))
		<< "the clock restarted from the beat, not from construction";
	EXPECT_TRUE(rule.poll(at_ns(nearly + POST_TRADE_TIMEOUT_NS + 1), 1));
}

TEST(PostTradeOutcomeSilence, ADisabledTimeoutNeverTrips) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(outcome_silence::NO_TIMEOUT),
						 at_ns(0)};

	EXPECT_FALSE(rule.poll(at_ns(1'000'000'000'000), 1000));
	EXPECT_FALSE(rule.is_silent(at_ns(1'000'000'000'000), 1000));
	EXPECT_EQ(rule.timeout_ns(), outcome_silence::NO_TIMEOUT);
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
}

TEST(PostTradeOutcomeSilence, AReadingBehindTheLastOutcomeIsNoSilenceAtAll) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(POST_TRADE_TIMEOUT_NS),
						 at_ns(5000)};

	// A caller may poll with a `now` it read before the outcome that has since
	// arrived. Clamping to zero is the truth in that ordering; wrapping would
	// be an enormous interval and a breaker tripped on a race with itself.
	EXPECT_EQ(rule.silence_ns(at_ns(1000)), 0U);
	EXPECT_FALSE(rule.poll(at_ns(1000), 10));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
}

TEST(PostTradeOutcomeSilence, OneOutageTripsOnce) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(POST_TRADE_TIMEOUT_NS),
						 at_ns(0)};

	const std::uint64_t past = POST_TRADE_TIMEOUT_NS * 2;
	EXPECT_TRUE(rule.poll(at_ns(past), 1));
	for (int i = 0; i < 10; ++i) EXPECT_FALSE(rule.poll(at_ns(past), 1));
	EXPECT_EQ(rule.trips(), 1U);
}

TEST(PostTradeOutcomeSilence, ARearmIntoAStillSilentPathTripsAgain) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(POST_TRADE_TIMEOUT_NS),
						 at_ns(0)};

	const std::uint64_t past = POST_TRADE_TIMEOUT_NS * 2;
	EXPECT_TRUE(rule.poll(at_ns(past), 1));

	breaker.arm();
	EXPECT_TRUE(rule.poll(at_ns(past), 1))
		<< "a re-arm into a condition that still holds does not buy a fresh "
		   "allowance";
	EXPECT_EQ(rule.trips(), 2U);
}

TEST(PostTradeOutcomeSilence, AMonitorNobodyPollsNeverFires) {
	circuit_breaker breaker;
	outcome_silence rule{breaker,
						 surveillance_silence(POST_TRADE_TIMEOUT_NS),
						 at_ns(0)};

	// The whole subject of this rule is an absence, and an absence delivers no
	// callback. Nothing here has been polled, so nothing has been noticed - the
	// contract, rather than an oversight.
	EXPECT_TRUE(rule.is_silent(at_ns(POST_TRADE_TIMEOUT_NS * 2), 1));
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_EQ(rule.trips(), 0U);
}

} // namespace
