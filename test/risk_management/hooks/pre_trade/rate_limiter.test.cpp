// The fixed-window counter, including the burst it admits at a boundary - that
// one is documented behaviour, so it is pinned rather than left to be
// rediscovered as a bug.

#include "risk_management/hooks/pre_trade/rate_limiter.hpp"

#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/detail/screening.hpp"
#include "risk_management/hooks/system/trading_state.hpp"
#include "risk_management/risk.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using exchange::risk::hooks::breach;
using exchange::risk::hooks::breach_set;
using exchange::risk::hooks::detail::screen_state;
using exchange::risk::hooks::pre_trade::rate_breach;
using exchange::risk::hooks::pre_trade::rate_limiter;
using exchange::risk::hooks::system::trading_state;

/// @brief A window of 2^10 ns, small enough to step across in a literal.
constexpr unsigned RATE_SMALL_WINDOW_LOG2 = 10;
constexpr std::uint64_t RATE_WINDOW_NS    = 1ull << RATE_SMALL_WINDOW_LOG2;

TEST(RiskRateLimiter, AFreshLimiterHasItsWholeAllowance) {
	const rate_limiter limiter{5, RATE_SMALL_WINDOW_LOG2};
	EXPECT_EQ(limiter.limit(), 5U);
	EXPECT_EQ(limiter.window_ns(), RATE_WINDOW_NS);
	EXPECT_EQ(limiter.used(at_ns(0)), 0U);
	EXPECT_EQ(limiter.headroom(at_ns(0)), 5U);
	EXPECT_TRUE(limiter.admits(at_ns(0), 5));
	EXPECT_FALSE(limiter.admits(at_ns(0), 6));
}

TEST(RiskRateLimiter, ChargingSpendsHeadroomWithinTheWindow) {
	rate_limiter limiter{5, RATE_SMALL_WINDOW_LOG2};
	limiter.charge(at_ns(0), 3);
	EXPECT_EQ(limiter.used(at_ns(0)), 3U);
	EXPECT_EQ(limiter.headroom(at_ns(0)), 2U);

	// Still the same window one nanosecond before it ends.
	EXPECT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS - 1)), 2U);
	EXPECT_FALSE(limiter.admits(at_ns(RATE_WINDOW_NS - 1), 3));
}

TEST(RiskRateLimiter, SpendingTheAllowanceLeavesNoHeadroom) {
	rate_limiter limiter{2, RATE_SMALL_WINDOW_LOG2};
	limiter.charge(at_ns(0), 2);
	EXPECT_EQ(limiter.headroom(at_ns(0)), 0U);
	EXPECT_FALSE(limiter.admits(at_ns(0), 1));
}

TEST(RiskRateLimiter, CrossingAWindowBoundaryRestoresTheAllowance) {
	rate_limiter limiter{2, RATE_SMALL_WINDOW_LOG2};
	limiter.charge(at_ns(0), 2);
	ASSERT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS - 1)), 0U);
	EXPECT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS)), 2U);
	EXPECT_EQ(limiter.used(at_ns(RATE_WINDOW_NS)), 0U);
}

TEST(RiskRateLimiter, ALimiterLeftAloneForManyWindowsReportsUnused) {
	rate_limiter limiter{4, RATE_SMALL_WINDOW_LOG2};
	limiter.charge(at_ns(0), 4);
	// No call in between: the rollover is discovered on the next question, not
	// noticed at the moment it happens.
	EXPECT_EQ(limiter.used(at_ns(RATE_WINDOW_NS * 1000)), 0U);
	EXPECT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS * 1000)), 4U);
}

TEST(RiskRateLimiter, AFixedWindowAdmitsTwiceTheLimitAcrossOneBoundary) {
	// Documented, deliberate, and the reason the default window is about a
	// millisecond: a burst bounded by two windows only matters if the window is
	// long. If this ever stops being true the class comment is wrong.
	rate_limiter limiter{3, RATE_SMALL_WINDOW_LOG2};
	limiter.charge(at_ns(RATE_WINDOW_NS - 1), 3);
	EXPECT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS - 1)), 0U);
	EXPECT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS)), 3U);
	limiter.charge(at_ns(RATE_WINDOW_NS), 3);
	// Six messages inside two nanoseconds, against a limit of three per window.
	EXPECT_EQ(limiter.used(at_ns(RATE_WINDOW_NS)), 3U);
}

TEST(RiskRateLimiter, ChargingPastTheLimitSaturatesRatherThanWrapping) {
	rate_limiter limiter{2, RATE_SMALL_WINDOW_LOG2};
	limiter.charge(at_ns(0), 2);
	limiter.charge(at_ns(0), 0xFFFF'FFFFU);
	// A wrap here would hand out a whole fresh window's allowance, which is the
	// one failure mode a throttle must not have.
	EXPECT_EQ(limiter.headroom(at_ns(0)), 0U);
}

TEST(RiskRateLimiter, AZeroAllowanceAdmitsNothing) {
	const rate_limiter limiter{0, RATE_SMALL_WINDOW_LOG2};
	EXPECT_EQ(limiter.headroom(at_ns(0)), 0U);
	EXPECT_FALSE(limiter.admits(at_ns(0), 1));
	EXPECT_TRUE(limiter.admits(at_ns(0), 0));
}

TEST(RiskRateLimiter, ResetForgetsTheCurrentWindow) {
	rate_limiter limiter{4, RATE_SMALL_WINDOW_LOG2};
	limiter.charge(at_ns(RATE_WINDOW_NS * 7), 4);
	ASSERT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS * 7)), 0U);
	limiter.reset();
	EXPECT_EQ(limiter.headroom(at_ns(RATE_WINDOW_NS * 7)), 4U);
}

TEST(RiskRateLimiter, AnOversizedWindowIsClampedRatherThanShiftingPastTheWord) {
	// A shift of 64 or more is undefined behaviour; the constructor clamps so a
	// misconfigured window is merely wide.
	const rate_limiter limiter{1, 999};
	EXPECT_EQ(limiter.window_ns(), 1ull << rate_limiter::MAX_WINDOW_LOG2_NS);
}

// --- the rule read against the window ------------------------------------
//
// One suite over both because they are one hook: the window answers "how much
// is left" once per batch, and these are the compare the gate makes per command
// against what the batch has spent since.

/// @brief A batch that has consumed nothing yet, with @p headroom to spend.
[[nodiscard]] screen_state opened_with(std::uint32_t headroom) {
	return {.now              = at_ns(0),
			.state            = trading_state::NORMAL,
			.headroom         = headroom,
			.base_net         = 0,
			.base_working_bid = 0,
			.base_working_ask = 0};
}

TEST(RiskRateLimiter, TheRuleMeasuresTheBatchAgainstTheHeadroom) {
	screen_state state = opened_with(2);
	EXPECT_EQ(rate_breach(state), 0U);

	// A batch is rate limited against itself: `charged` is what the commands
	// ahead of this one in the same call have already committed, which is the
	// case that matters because a loop does not pause to let a window roll.
	state.charged = 1;
	EXPECT_EQ(rate_breach(state), 0U);
	state.charged = 2;
	EXPECT_NE(rate_breach(state), 0U);
	EXPECT_TRUE(
		breach_set::from_bits(rate_breach(state)).test(breach::MESSAGE_RATE));
}

TEST(RiskRateLimiter, NoHeadroomAtAllRefusesTheFirstCommand) {
	// The state a spent window leaves: nothing charged yet this batch, and no
	// allowance to charge it against.
	EXPECT_NE(rate_breach(opened_with(0)), 0U);
}
} // namespace
