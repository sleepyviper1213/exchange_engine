// The fixed-window counter, including the burst it admits at a boundary - that
// one is documented behaviour, so it is pinned rather than left to be
// rediscovered as a bug.

#include "risk_management/rate_limiter.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using exchange::risk::rate_limiter;

/// @brief A window of 2^10 ns, small enough to step across in a literal.
constexpr unsigned SMALL_WINDOW_LOG2 = 10;
constexpr std::uint64_t WINDOW_NS    = std::uint64_t{1} << SMALL_WINDOW_LOG2;

TEST(RiskRateLimiter, AFreshLimiterHasItsWholeAllowance) {
	const rate_limiter limiter{5, SMALL_WINDOW_LOG2};
	EXPECT_EQ(limiter.limit(), 5U);
	EXPECT_EQ(limiter.window_ns(), WINDOW_NS);
	EXPECT_EQ(limiter.used(0), 0U);
	EXPECT_EQ(limiter.headroom(0), 5U);
	EXPECT_TRUE(limiter.admits(0, 5));
	EXPECT_FALSE(limiter.admits(0, 6));
}

TEST(RiskRateLimiter, ChargingSpendsHeadroomWithinTheWindow) {
	rate_limiter limiter{5, SMALL_WINDOW_LOG2};
	limiter.charge(0, 3);
	EXPECT_EQ(limiter.used(0), 3U);
	EXPECT_EQ(limiter.headroom(0), 2U);

	// Still the same window one nanosecond before it ends.
	EXPECT_EQ(limiter.headroom(WINDOW_NS - 1), 2U);
	EXPECT_FALSE(limiter.admits(WINDOW_NS - 1, 3));
}

TEST(RiskRateLimiter, SpendingTheAllowanceLeavesNoHeadroom) {
	rate_limiter limiter{2, SMALL_WINDOW_LOG2};
	limiter.charge(0, 2);
	EXPECT_EQ(limiter.headroom(0), 0U);
	EXPECT_FALSE(limiter.admits(0, 1));
}

TEST(RiskRateLimiter, CrossingAWindowBoundaryRestoresTheAllowance) {
	rate_limiter limiter{2, SMALL_WINDOW_LOG2};
	limiter.charge(0, 2);
	ASSERT_EQ(limiter.headroom(WINDOW_NS - 1), 0U);
	EXPECT_EQ(limiter.headroom(WINDOW_NS), 2U);
	EXPECT_EQ(limiter.used(WINDOW_NS), 0U);
}

TEST(RiskRateLimiter, ALimiterLeftAloneForManyWindowsReportsUnused) {
	rate_limiter limiter{4, SMALL_WINDOW_LOG2};
	limiter.charge(0, 4);
	// No call in between: the rollover is discovered on the next question, not
	// noticed at the moment it happens.
	EXPECT_EQ(limiter.used(WINDOW_NS * 1000), 0U);
	EXPECT_EQ(limiter.headroom(WINDOW_NS * 1000), 4U);
}

TEST(RiskRateLimiter, AFixedWindowAdmitsTwiceTheLimitAcrossOneBoundary) {
	// Documented, deliberate, and the reason the default window is about a
	// millisecond: a burst bounded by two windows only matters if the window is
	// long. If this ever stops being true the class comment is wrong.
	rate_limiter limiter{3, SMALL_WINDOW_LOG2};
	limiter.charge(WINDOW_NS - 1, 3);
	EXPECT_EQ(limiter.headroom(WINDOW_NS - 1), 0U);
	EXPECT_EQ(limiter.headroom(WINDOW_NS), 3U);
	limiter.charge(WINDOW_NS, 3);
	// Six messages inside two nanoseconds, against a limit of three per window.
	EXPECT_EQ(limiter.used(WINDOW_NS), 3U);
}

TEST(RiskRateLimiter, ChargingPastTheLimitSaturatesRatherThanWrapping) {
	rate_limiter limiter{2, SMALL_WINDOW_LOG2};
	limiter.charge(0, 2);
	limiter.charge(0, 0xFFFF'FFFFU);
	// A wrap here would hand out a whole fresh window's allowance, which is the
	// one failure mode a throttle must not have.
	EXPECT_EQ(limiter.headroom(0), 0U);
}

TEST(RiskRateLimiter, AZeroAllowanceAdmitsNothing) {
	const rate_limiter limiter{0, SMALL_WINDOW_LOG2};
	EXPECT_EQ(limiter.headroom(0), 0U);
	EXPECT_FALSE(limiter.admits(0, 1));
	EXPECT_TRUE(limiter.admits(0, 0));
}

TEST(RiskRateLimiter, ResetForgetsTheCurrentWindow) {
	rate_limiter limiter{4, SMALL_WINDOW_LOG2};
	limiter.charge(WINDOW_NS * 7, 4);
	ASSERT_EQ(limiter.headroom(WINDOW_NS * 7), 0U);
	limiter.reset();
	EXPECT_EQ(limiter.headroom(WINDOW_NS * 7), 4U);
}

TEST(RiskRateLimiter, AnOversizedWindowIsClampedRatherThanShiftingPastTheWord) {
	// A shift of 64 or more is undefined behaviour; the constructor clamps so a
	// misconfigured window is merely wide.
	const rate_limiter limiter{1, 999};
	EXPECT_EQ(limiter.window_ns(),
			  std::uint64_t{1} << rate_limiter::MAX_WINDOW_LOG2_NS);
}

} // namespace
