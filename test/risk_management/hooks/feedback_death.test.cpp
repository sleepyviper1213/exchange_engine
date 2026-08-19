// The router's wiring contracts, which only fire while assertions are live. Kept
// in their own file because a suite that forks should not share a binary with one
// that does not. @see testing.md
//
// Both are preconditions in the CLAUDE.md sense - things the *deployment*
// guarantees when it wires a topology up, not input a running system can send.
// Nothing on the event path can reach either: `attach` is called once per gate at
// startup, and the events themselves are routed by lookup, which misses quietly
// on purpose.

#include "hooks.fixture.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

using namespace exchange;
using namespace exchange::risk;
using testing::HasSubstr;

namespace {

// Two gates fed for one listing would each apply every fill, so the position
// book would move twice per print and every later exposure check would be wrong
// in the permissive direction. Stacked gates are stacked on the way *in*, where
// the inner one's sink is the outer one, and only the innermost is fed.
TEST(RiskHooksFeedbackDeath, TwoGatesForOneListingIsAContractViolation) {
	feedback_desk desk{SYMBOL}; // both gates screen SYMBOL
	desk.router().attach(desk.first());

	// DEBUG_DEATH rather than DEATH: overwriting the slot double-counts fills,
	// which is a wrong answer rather than an out-of-bounds access, so running the
	// statement in-process is safe.
	EXPECT_DEBUG_DEATH(desk.router().attach(desk.second()),
					   HasSubstr("one listing, one gate"));
}

// A gate whose listing is past the table would be written outside the vector.
TEST(RiskHooksFeedbackDeath, AListingPastTheTableIsAContractViolation) {
	feedback_desk desk{UNSCREENED_SYMBOL, /*listings=*/4};

#ifdef NDEBUG
	GTEST_SKIP()
		<< "assertions are compiled out (ORDER_BOOK_ENABLE_HARDENING is "
		   "OFF and this configuration defines NDEBUG); executing the "
		   "statement would be an out-of-bounds write, not a wrong answer";
#else
	EXPECT_DEATH(desk.router().attach(desk.second()),
				 HasSubstr("sized for this listing"));
#endif
}

// The counterpart, and the reason the two are different: an *event* for a listing
// nobody screens is ordinary - a partition publishes everything it matched - so it
// is counted and consumed rather than asserted on.
TEST(RiskHooksFeedbackDeath, AnEventForAnUnattachedListingIsNotAViolation) {
	feedback_desk desk;
	desk.router().attach(desk.first());
	const std::array fills{filled_at(1, 2, 100, 1)};

	EXPECT_EQ(desk.router().on_trades(OTHER_SYMBOL, fills), 1U);
	EXPECT_EQ(desk.router().unrouted(), 1U);
}

} // namespace
