// When a live session pulls its own book, and when it deliberately does not.
//
// `risk_gate_mass_cancel.test.cpp` pins the walk itself. What is left to pin is
// the policy in front of it, and the two ways it is easy to get wrong:
//
//   * firing on a *state* rather than on a transition, which re-sends the whole
//     ledger on every pump for as long as the breaker stays open - a message
//     storm aimed at the venue that just decided something was wrong with us;
//   * firing on CANCEL_ONLY under ON_HALT, which pulls a still-trusted
//     strategy's quotes out from under it. CANCEL_ONLY means "stop adding",
//     not "you are not to be trusted".
//
// What failure looks like: an operator picks ON_HALT expecting a drawdown trip
// to leave the book alone, and the session withdraws everything.

#include "live_session.fixture.hpp"
#include "risk_management/hooks/system/trading_state.hpp"
#include "session/live_session.hpp"

#include <gtest/gtest.h>

namespace {

using exchange::risk::hooks::system::trading_state;
using exchange::session::mass_cancel_policy;

/// @brief Options naming nothing but the policy under test.
[[nodiscard]] live_session_options
live_mass_cancel_options(mass_cancel_policy policy) {
	return live_session_options{.mass_cancel = policy};
}

/// @brief Quote a two-sided market, so the gate has something to withdraw.
///
/// A function taking the desk rather than returning one: `live_session` holds a
/// `risk_gate`, which deletes its move constructor because there is nowhere for
/// a gate pinned to a producer thread to go - so a desk cannot be returned by
/// value and NRVO has nothing to elide.
void live_mass_cancel_arm(live_desk &desk) {
	ASSERT_TRUE(desk.seed_touch());
	ASSERT_EQ(desk.session().gate().working_orders(), 2U)
		<< "the policy cases are meaningless without orders to withdraw";
}

TEST(AppLiveSessionMassCancel, ManualLeavesTheBookAloneOnEveryTrip) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::MANUAL)};
	live_mass_cancel_arm(desk);

	desk.session().breaker().trip(trading_state::HALTED);
	desk.advance(0);

	EXPECT_EQ(desk.report().mass_cancels, 0U);
	EXPECT_EQ(desk.report().mass_cancelled, 0U);
}

TEST(AppLiveSessionMassCancel, ManualStillWithdrawsWhenAskedDirectly) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::MANUAL)};
	live_mass_cancel_arm(desk);

	// The whole point of MANUAL: the capability is there, the automation is
	// not. An operator's console calls exactly this.
	EXPECT_EQ(desk.session().mass_cancel(), 2U);
	EXPECT_EQ(desk.report().mass_cancelled, 2U);
	// Not counted as a policy firing, because no policy fired it.
	EXPECT_EQ(desk.report().mass_cancels, 0U);
}

TEST(AppLiveSessionMassCancel, OnHaltIgnoresTheAutomaticCancelOnlyTrip) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::ON_HALT)};
	live_mass_cancel_arm(desk);

	// What every automatic trip selects - a drawdown, a stale feed, a looping
	// strategy. The strategy is still trusted to manage its own orders.
	desk.session().breaker().trip(trading_state::CANCEL_ONLY);
	desk.advance(0);

	EXPECT_EQ(desk.report().mass_cancels, 0U);
	EXPECT_EQ(desk.session().gate().working_orders(), 2U);
}

TEST(AppLiveSessionMassCancel,
	 OnHaltWithdrawsWhenTheStrategyIsNoLongerTrusted) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::ON_HALT)};
	live_mass_cancel_arm(desk);

	desk.session().breaker().trip(trading_state::HALTED);
	desk.advance(0);

	EXPECT_EQ(desk.report().mass_cancels, 1U);
	EXPECT_EQ(desk.report().mass_cancelled, 2U);
}

TEST(AppLiveSessionMassCancel, OnAnyTripWithdrawsOnCancelOnlyToo) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::ON_ANY_TRIP)};
	live_mass_cancel_arm(desk);

	desk.session().breaker().trip(trading_state::CANCEL_ONLY);
	desk.advance(0);

	EXPECT_EQ(desk.report().mass_cancels, 1U);
	EXPECT_EQ(desk.report().mass_cancelled, 2U);
}

TEST(AppLiveSessionMassCancel, ATripFiresOnceHoweverManyTimesTheLoopPumps) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::ON_ANY_TRIP)};
	live_mass_cancel_arm(desk);

	desk.session().breaker().trip(trading_state::HALTED);
	for (int pump = 0; pump < 10; ++pump) desk.advance(0);

	// The ledger is deliberately not retired by a mass cancel, so a policy that
	// tested the state rather than the transition would find work to do on
	// every one of those pumps and re-send both cancels each time.
	EXPECT_EQ(desk.report().mass_cancels, 1U);
	EXPECT_EQ(desk.report().mass_cancelled, 2U);
}

TEST(AppLiveSessionMassCancel, ARearmedBreakerFiresAgainOnTheNextTrip) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::ON_HALT)};
	live_mass_cancel_arm(desk);

	desk.session().breaker().trip(trading_state::HALTED);
	desk.advance(0);
	ASSERT_EQ(desk.report().mass_cancels, 1U);

	// Going back to NORMAL is a transition too, and not one that withdraws.
	desk.session().breaker().arm();
	desk.advance(0);
	EXPECT_EQ(desk.report().mass_cancels, 1U);

	desk.session().breaker().trip(trading_state::HALTED);
	desk.advance(0);
	EXPECT_EQ(desk.report().mass_cancels, 2U);
	EXPECT_EQ(desk.report().mass_cancelled, 4U);
}

TEST(AppLiveSessionMassCancel, AnEmptyLedgerFiresThePolicyAndSendsNothing) {
	live_desk desk{live_mass_cancel_options(mass_cancel_policy::ON_HALT)};
	ASSERT_EQ(desk.session().gate().working_orders(), 0U);

	desk.session().breaker().trip(trading_state::HALTED);
	desk.advance(0);

	// The transition is what fires, so it is counted; there was simply nothing
	// to withdraw. Reporting them separately is what lets an operator tell "the
	// policy never ran" from "it ran and the book was already flat".
	EXPECT_EQ(desk.report().mass_cancels, 1U);
	EXPECT_EQ(desk.report().mass_cancelled, 0U);
}

} // namespace
