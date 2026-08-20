// The wiring mistakes the router refuses outright.
//
// In its own file because a suite that forks has to stay isolated from one that
// does not. @see feedback_death.test.cpp, which pins the gate half of the same
// contract.
//
// Both of these are asserts rather than return values on purpose: attaching is
// deployment-time wiring, so a wrong one is a bug in the composition root that
// should stop the process, not a condition an event path checks for. A monitor
// fed the wrong listing's prints would count executions the account never had
// and time a return path that is not its own, and neither shows up as anything
// but a strategy behaving oddly hours later.
//
// `EXPECT_DEBUG_DEATH` rather than `EXPECT_DEATH` throughout, for the reason
// `feedback_death.test.cpp` gives: configurations that define `NDEBUG` compile
// the assertions out, and the statement has to be safe to run in-process when
// they do. Both of these are - a misrouted monitor counts the wrong listing's
// prints and a second one is written over the first, which are wrong answers
// rather than out-of-bounds writes. The suite that cannot make that promise is
// the one asserting on a listing past the table, and it skips instead.

#include "post_trade.fixture.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using testing::HasSubstr;

namespace {

using namespace exchange;
using namespace exchange::risk;
using namespace exchange::risk::hooks;

TEST(PostTradeRoutingDeath, RefusesAMonitorForAnotherListing) {
	position_book positions{post_trade_desk::LISTINGS};
	circuit_breaker breaker;
	manual_clock clock;
	recording_sink sink;
	test_gate gate{sink, SYMBOL, permissive(), positions, breaker, 0, clock};

	// The monitor watches OTHER_SYMBOL; the gate screens SYMBOL. Feeding this
	// pair would move a ratio and a tape that belong to a different
	// instrument.
	post_trade_monitor elsewhere{breaker, OTHER_SYMBOL, surveillance(), 0};
	post_trade_router router{post_trade_desk::LISTINGS, clock};

	EXPECT_DEBUG_DEATH(router.attach(gate, elsewhere),
					   HasSubstr("watches the listing its gate screens"));
}

TEST(PostTradeRoutingDeath, RefusesASecondMonitorForOneListing) {
	position_book positions{post_trade_desk::LISTINGS};
	circuit_breaker breaker;
	manual_clock clock;
	recording_sink sink;
	test_gate gate{sink, SYMBOL, permissive(), positions, breaker, 0, clock};

	post_trade_monitor first{breaker, SYMBOL, surveillance(), 0};
	post_trade_monitor second{breaker, SYMBOL, surveillance(), 0};
	post_trade_router router{post_trade_desk::LISTINGS, clock};

	router.attach(gate, first);
	// Refused by the gate half of attach: one listing, one gate - and therefore
	// one monitor beside it.
	EXPECT_DEBUG_DEATH(router.attach(gate, second),
					   HasSubstr("one listing, one gate"));
}

} // namespace
