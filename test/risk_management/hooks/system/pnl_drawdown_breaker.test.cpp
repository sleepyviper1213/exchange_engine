// The loss floor as a rule rather than as a gate behaviour.
//
// `gate/loss_limit.test.cpp` owns the end-to-end story - a fill that realises a
// loss, a market that moves against a position nobody touched. What is only
// visible here is the *laziness*: the profit arrives as a callable so that the
// two conditions which rule the check out never pay for reading a number several
// threads write to.

#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management/hooks/system/pnl_drawdown_breaker.hpp"
#include "risk_management/limits.hpp"
#include "risk_management/hooks/system/trading_state.hpp"

#include <gtest/gtest.h>

#include <cstdint>


namespace {

using namespace exchange::risk;
using namespace exchange::risk::hooks::system;

/// @brief Limits that stop trading once @p loss tick-lots have been lost.
[[nodiscard]] risk_limits with_floor(std::int64_t loss) {
	risk_limits limits = risk_limits{};
	limits.max_loss    = loss;
	return limits;
}

/// @brief A profit reading that counts how often it was asked for.
class counted_pnl {
public:
	counted_pnl(std::int64_t value, int &reads) noexcept
		: value_(value), reads_(&reads) {}

	std::int64_t operator()() const noexcept {
		++*reads_;
		return value_;
	}

private:
	std::int64_t value_;
	int *reads_;
};

TEST(RiskHooksDrawdown, TheFloorIsTheLastAdmissibleValue) {
	const risk_limits limits = with_floor(500);
	EXPECT_FALSE(through_floor(0, limits));
	EXPECT_FALSE(through_floor(-499, limits));
	EXPECT_FALSE(through_floor(-500, limits)); // exactly the floor is inside it
	EXPECT_TRUE(through_floor(-501, limits));
}

TEST(RiskHooksDrawdown, NoFloorConfiguredIsNeverThroughIt) {
	// Zero is "no limit" rather than "a floor at zero", so a strategy that has
	// lost everything is still not in breach of a limit nobody set.
	EXPECT_FALSE(through_floor(-1'000'000, risk_limits{}));
}

TEST(RiskHooksDrawdown, FallingThroughTheFloorTripsToCancelOnly) {
	circuit_breaker breaker;
	int reads = 0;

	EXPECT_TRUE(
		trip_on_drawdown(breaker, with_floor(500), counted_pnl{-501, reads}));

	EXPECT_EQ(breaker.state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(breaker.cause(), trip_cause::LOSS_LIMIT);
	EXPECT_TRUE(breaker.passes_cancels()); // shedding the position is the point
	EXPECT_EQ(reads, 1);
}

TEST(RiskHooksDrawdown, ATrippedBreakerIsNotTrippedAgain) {
	// One trip and one cause however far it bleeds - an operator counting trips
	// wants events, not ticks.
	circuit_breaker breaker;
	int reads = 0;
	ASSERT_TRUE(
		trip_on_drawdown(breaker, with_floor(500), counted_pnl{-501, reads}));

	EXPECT_FALSE(
		trip_on_drawdown(breaker, with_floor(500), counted_pnl{-9'999, reads}));
	EXPECT_EQ(breaker.trips(), 1U);
}

TEST(RiskHooksDrawdown, TheProfitIsNotEvenReadWhenTheCheckCannotFire) {
	// The reason the parameter is a callable. Reading profit touches a line other
	// threads write, and this runs on every print of every session - including the
	// sessions that configure no floor at all.
	circuit_breaker breaker;
	int reads = 0;

	EXPECT_FALSE(
		trip_on_drawdown(breaker, risk_limits{}, counted_pnl{-1'000, reads}));
	EXPECT_EQ(reads, 0) << "no floor configured: nothing to compare against";

	breaker.trip(trading_state::CANCEL_ONLY);
	EXPECT_FALSE(
		trip_on_drawdown(breaker, with_floor(1), counted_pnl{-1'000, reads}));
	EXPECT_EQ(reads, 0) << "already open: the answer cannot change anything";
}

} // namespace
