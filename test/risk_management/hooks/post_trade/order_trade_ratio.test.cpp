// The rule that catches a strategy talking without trading, and the counting
// policy that decides what "talking" means.
//
// Two halves worth keeping apart. The arithmetic is a pure function and is
// pinned at compile time, including the two boundaries that make it a rule
// rather than a division: the floor below which a ratio is meaningless, and the
// cap being the last admissible value rather than the first refused one. The
// counting is a judgement about what a venue would charge us for, and every
// outcome shape gets an assertion because the whole rule is only as honest as
// its numerator.

#include "post_trade.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::risk;
using namespace exchange::risk::hooks::post_trade;

// --- the arithmetic, at compile time -------------------------------------

static_assert(!is_over_ratio(1'000'000, 0, 0, 1),
			  "a cap of zero disables the rule, whatever the counts say");

static_assert(!is_over_ratio(99, 0, 1, 100),
			  "under the floor the ratio is not judged at all");

static_assert(is_over_ratio(100, 0, 1, 100),
			  "at the floor it is, and no executions means any cap is over");

static_assert(!is_over_ratio(100, 50, 2, 10),
			  "exactly the cap is the last admissible value, not the first "
			  "refused one");

static_assert(is_over_ratio(101, 50, 2, 10),
			  "one message past it is the first refused one");

// The multiplied form's whole point: no special case for an empty denominator.
static_assert(is_over_ratio(10, 0, 1, 10),
			  "no executions is a ratio nothing satisfies, not a division by "
			  "zero");

// --- what counts as a message --------------------------------------------

TEST(PostTradeOrderTradeRatio, CountsEveryMessageTheVenueAnswered) {
	EXPECT_TRUE(is_venue_message(post_trade_ack(1)));
	EXPECT_TRUE(is_venue_message(post_trade_reject(2)))
		<< "the venue parsed it and said no - that is not a discount";
	EXPECT_TRUE(is_venue_message(withdrawn(3, 10)))
		<< "a client cancel is a message";
	EXPECT_TRUE(is_venue_message(post_trade_cancel_reject(4)))
		<< "a cancel it could not apply is still a cancel it received";
}

TEST(PostTradeOrderTradeRatio, CountsNeitherFillsNorTimeInForceDrops) {
	EXPECT_FALSE(is_venue_message(filled(1, 10)))
		<< "a fill is the venue talking to us, which is the other direction";
	EXPECT_FALSE(is_venue_message(post_trade_ioc_drop(2, 10)))
		<< "nobody sent a cancel for an IOC remainder the book withdrew";
}

// --- what the rule does with them ----------------------------------------

TEST(PostTradeOrderTradeRatio, TripsOnTheMessageThatCrossesTheRatio) {
	circuit_breaker breaker;
	order_trade_ratio rule{breaker, surveillance_ratio(1, 3)};

	EXPECT_FALSE(rule.record_message(at_ns(0)));
	EXPECT_FALSE(rule.record_message(at_ns(0))) << "still under the floor";
	EXPECT_TRUE(rule.record_message(at_ns(0)))
		<< "at the floor, with nothing traded";

	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(breaker.cause(), trip_cause::ORDER_TRADE_RATIO);
	EXPECT_EQ(rule.trips(), 1U);
}

TEST(PostTradeOrderTradeRatio, ExecutionsBuyMessages) {
	circuit_breaker breaker;
	order_trade_ratio rule{breaker, surveillance_ratio(2, 4)};

	rule.record_execution(at_ns(0));
	rule.record_execution(at_ns(0));
	for (int i = 0; i < 4; ++i) EXPECT_FALSE(rule.record_message(at_ns(0)));

	EXPECT_EQ(rule.messages(at_ns(0)), 4U);
	EXPECT_EQ(rule.executions(at_ns(0)), 2U);
	EXPECT_FALSE(rule.is_breaching(at_ns(0)))
		<< "four messages against two executions is exactly 2:1";

	EXPECT_TRUE(rule.record_message(at_ns(0))) << "the fifth is not";
	EXPECT_EQ(breaker.cause(), trip_cause::ORDER_TRADE_RATIO);
}

TEST(PostTradeOrderTradeRatio, ANewWindowStartsFromNothing) {
	circuit_breaker breaker;
	order_trade_ratio rule{breaker, surveillance_ratio(1, 2)};

	EXPECT_FALSE(rule.record_message(at_ns(0)));
	EXPECT_EQ(rule.messages(at_ns(0)), 1U);

	// One window on, and the stored count belongs to an epoch that has gone.
	EXPECT_EQ(rule.messages(at_ns(POST_TRADE_WINDOW_NS)), 0U)
		<< "a window is forgotten by being asked about, with no rollover call";
	EXPECT_FALSE(rule.record_message(at_ns(POST_TRADE_WINDOW_NS)))
		<< "so this is the first message of the new window, not the second";
	EXPECT_EQ(rule.messages(at_ns(POST_TRADE_WINDOW_NS)), 1U);
	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
}

TEST(PostTradeOrderTradeRatio, ADisabledCapNeverTrips) {
	circuit_breaker breaker;
	order_trade_ratio rule{breaker,
						   surveillance_ratio(order_trade_ratio::NO_LIMIT, 1)};

	for (int i = 0; i < 1000; ++i) EXPECT_FALSE(rule.record_message(at_ns(0)));

	EXPECT_EQ(breaker.state(), trading_state::NORMAL);
	EXPECT_EQ(rule.trips(), 0U);
	EXPECT_EQ(rule.total_messages(), 1000U)
		<< "disabled means it does not act, not that it stops counting";
}

TEST(PostTradeOrderTradeRatio, OneRunawayTripsOnce) {
	circuit_breaker breaker;
	order_trade_ratio rule{breaker, surveillance_ratio(1, 1)};

	EXPECT_TRUE(rule.record_message(at_ns(0)));
	for (int i = 0; i < 50; ++i) EXPECT_FALSE(rule.record_message(at_ns(0)));

	EXPECT_EQ(rule.trips(), 1U)
		<< "an open breaker is left alone - one episode, one cause";
	EXPECT_EQ(rule.total_messages(), 51U);
}

TEST(PostTradeOrderTradeRatio, TotalsSpanWindowsAndTheWindowedCountsDoNot) {
	circuit_breaker breaker;
	order_trade_ratio rule{breaker,
						   surveillance_ratio(order_trade_ratio::NO_LIMIT, 1)};

	std::uint64_t now = 0;
	for (int window = 0; window < 3; ++window) {
		rule.record_message(at_ns(now));
		rule.record_execution(at_ns(now));
		now += POST_TRADE_WINDOW_NS;
	}

	EXPECT_EQ(rule.total_messages(), 3U);
	EXPECT_EQ(rule.total_executions(), 3U);
	EXPECT_EQ(rule.messages(at_ns(now)), 0U)
		<< "the session view and the window view "
		   "answer different questions";
	EXPECT_EQ(rule.window_ns(), POST_TRADE_WINDOW_NS);
}

TEST(PostTradeOrderTradeRatio, ExecutionsAloneNeverTrip) {
	circuit_breaker breaker;
	order_trade_ratio rule{breaker, surveillance_ratio(1, 1)};

	for (int i = 0; i < 100; ++i) rule.record_execution(at_ns(0));

	EXPECT_EQ(breaker.state(), trading_state::NORMAL)
		<< "trading is what the rule wants more of";
	EXPECT_FALSE(rule.is_breaching(at_ns(0)));
}

} // namespace
