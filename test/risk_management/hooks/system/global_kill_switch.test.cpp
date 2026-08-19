// What a tripped breaker refuses, at the rule level.
//
// `gate/screening.test.cpp` and `gate/loss_limit.test.cpp` drive these through a
// gate. What is worth pinning here is the *asymmetry* between the two rules,
// because it is the whole design of the kill switch and it is easy to "simplify"
// into a single state compare that would freeze a malfunctioning strategy's
// orders in the book.

#include "risk_management/hooks/system/global_kill_switch.hpp"
#include "risk_management/hooks/system/trading_state.hpp"

#include <gtest/gtest.h>


namespace {

using namespace exchange::risk;
using namespace exchange::risk::hooks::system;

TEST(RiskHooksKillSwitch, NewLiquidityNeedsAFullyNormalVenue) {
	EXPECT_EQ(new_liquidity_breach(trading_state::NORMAL), 0U);
	EXPECT_NE(new_liquidity_breach(trading_state::CANCEL_ONLY), 0U);
	EXPECT_NE(new_liquidity_breach(trading_state::HALTED), 0U);
}

TEST(RiskHooksKillSwitch, RiskReducingCommandsSurviveEveryAutomaticTrip) {
	// If this ever matches the rule above, the automatic trips have started
	// freezing positions instead of stopping them growing.
	EXPECT_EQ(risk_reducing_breach(trading_state::NORMAL), 0U);
	EXPECT_EQ(risk_reducing_breach(trading_state::CANCEL_ONLY), 0U);
	EXPECT_NE(risk_reducing_breach(trading_state::HALTED), 0U);
}

TEST(RiskHooksKillSwitch, CancelOnlyStopsOrdersAndNotWithdrawals) {
	// The two rules disagree on exactly one state, and that state is the one the
	// drawdown breaker, the breach-rate cut-out and the heartbeat monitor all
	// choose.
	EXPECT_NE(new_liquidity_breach(trading_state::CANCEL_ONLY), 0U);
	EXPECT_EQ(risk_reducing_breach(trading_state::CANCEL_ONLY), 0U);
}

TEST(RiskHooksKillSwitch, TheReasonReportedIsHaltedAndNothingElse) {
	// One bit, so a client is told the venue is not accepting this rather than
	// something about their order.
	const breach_set refused =
		breach_set::from_bits(new_liquidity_breach(trading_state::HALTED));
	EXPECT_TRUE(refused.test(breach::HALTED));
	EXPECT_EQ(refused.count(), 1U);
	EXPECT_EQ(first_reason(refused), exchange::engine::reject_reason::RISK_HALTED);
}

} // namespace
