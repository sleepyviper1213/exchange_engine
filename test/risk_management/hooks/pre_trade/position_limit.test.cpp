// The projections: this order against the position, and the worse of the two
// hypotheticals against the exposure limit.
//
// The gross rule is the one worth pinning here, because summing the two sides
// instead of taking the worse of them is the obvious implementation and it is
// wrong: a quoter showing ten up and ten down cannot end up twenty long, so
// summing would halve its allowance for a position it can never hold.

#include "pre_trade.fixture.hpp"
#include "risk_management/hooks/pre_trade/position_limit.hpp"

#include <gtest/gtest.h>


namespace {

using namespace exchange;
using namespace exchange::risk;
using namespace exchange::risk::hooks::pre_trade;

TEST(RiskHooksPositionLimit, TheNetRuleMeasuresThisOrderAgainstThePosition) {
	risk_limits limits       = risk_limits{};
	limits.max_position_lots = 10;
	screen_state state       = fresh();

	EXPECT_EQ(exposure_breaches(buy(1, 100, 10), state, limits, 100), 0U);
	EXPECT_TRUE(breach_set::from_bits(
					exposure_breaches(buy(1, 100, 11), state, limits, 100))
					.test(breach::POSITION_LIMIT));

	// Already ten long: one more lot is over.
	state.base_net = 10;
	EXPECT_TRUE(breach_set::from_bits(
					exposure_breaches(buy(2, 100, 1), state, limits, 100))
					.test(breach::POSITION_LIMIT));
	// And the rule is one-sided, so the same order the other way reduces and is
	// admitted. A risk check that refused it would trap a strategy in a position
	// it is trying to shed.
	EXPECT_EQ(exposure_breaches(sell(3, 100, 1), state, limits, 100), 0U);
}

TEST(RiskHooksPositionLimit, WorkingOrdersCountBeforeAnythingFills) {
	// The whole reason exposure is measured at submission: an order that has been
	// sent is exposure whether or not it has filled, and a check that looked only
	// at the filled position would let a strategy build any position it liked out
	// of orders in flight.
	risk_limits limits           = risk_limits{};
	limits.max_exposure_notional = 1'000; // ten lots at a mark of 100
	screen_state state           = fresh();
	state.base_working_bid       = 10;

	EXPECT_TRUE(breach_set::from_bits(
					exposure_breaches(buy(1, 100, 1), state, limits, 100))
					.test(breach::EXPOSURE_LIMIT));
}

TEST(RiskHooksPositionLimit, GrossExposureTakesTheWorseSideRatherThanTheSum) {
	risk_limits limits           = risk_limits{};
	limits.max_exposure_notional = 1'000;
	screen_state state           = fresh();
	state.base_working_bid       = 10;

	// Ten up and ten down: gross is ten, not twenty, so this is admitted.
	EXPECT_EQ(exposure_breaches(sell(1, 100, 10), state, limits, 100), 0U);
	// Eleven on the worse side is over, which is what proves the limit is live
	// rather than the test passing because nothing is being measured.
	EXPECT_TRUE(breach_set::from_bits(
					exposure_breaches(sell(2, 100, 11), state, limits, 100))
					.test(breach::EXPOSURE_LIMIT));
}

TEST(RiskHooksPositionLimit, ExposureValuesAtZeroBeforeTheFirstPrint) {
	// An unknown mark cannot value anything, and refusing every order until one
	// arrives would be the wrong answer to "we do not know yet".
	risk_limits limits           = risk_limits{};
	limits.max_exposure_notional = 1;
	EXPECT_EQ(exposure_breaches(buy(1, 100, 1'000), fresh(), limits, 0), 0U);
}

} // namespace
