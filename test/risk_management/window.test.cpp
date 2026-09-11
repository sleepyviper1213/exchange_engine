// The one operation in the risk module that takes the scalar out of a
// monotonic_time, and the property both its callers depend on.
//
// `rate_limiter` and `circuit_breaker` both bucket time into fixed windows and
// both keep only the *current* window's count, so the only question either asks
// of a window index is "is it the same one as last time". That makes equality
// across a boundary the whole contract, and it is worth pinning separately from
// either hook: a shift that was off by one would still make both of their own
// suites pass for every case that does not straddle an edge.

#include "risk_management/window.hpp"

#include "core/chrono/clock.hpp"
#include "risk.fixture.hpp" // at_ns

#include <gtest/gtest.h>

#include <cstdint>

using exchange::core::chrono::monotonic_clock;
using exchange::core::chrono::monotonic_time;
using exchange::risk::window_of;

namespace {

/// A 1024 ns window - small enough to write both sides of an edge as literals.
constexpr unsigned RISK_CLOCK_LOG2       = 10;
constexpr std::uint64_t RISK_CLOCK_WIDTH = 1ull << RISK_CLOCK_LOG2;

} // namespace

TEST(RiskWindow, InstantsInsideOneWindowShareItsIndex) {
	// The case both hooks are built on: readings a few nanoseconds apart must
	// accumulate against one count rather than resetting it.
	EXPECT_EQ(window_of(at_ns(0), RISK_CLOCK_LOG2),
			  window_of(at_ns(1), RISK_CLOCK_LOG2));
	EXPECT_EQ(window_of(at_ns(0), RISK_CLOCK_LOG2),
			  window_of(at_ns(RISK_CLOCK_WIDTH - 1), RISK_CLOCK_LOG2));
}

TEST(RiskWindow, TheBoundaryStartsANewWindow) {
	// Off by one here and a breach counter would carry one window too far,
	// which is the failure neither hook's own suite would catch away from an
	// edge.
	EXPECT_NE(window_of(at_ns(RISK_CLOCK_WIDTH - 1), RISK_CLOCK_LOG2),
			  window_of(at_ns(RISK_CLOCK_WIDTH), RISK_CLOCK_LOG2));
	EXPECT_EQ(window_of(at_ns(RISK_CLOCK_WIDTH), RISK_CLOCK_LOG2),
			  window_of(at_ns(RISK_CLOCK_WIDTH + 1), RISK_CLOCK_LOG2));
}

TEST(RiskWindow, ConsecutiveWindowsAreConsecutiveIndices) {
	// Not part of the contract either hook needs - both only compare for
	// equality - but it is what makes the value readable in a debugger, and a
	// change that broke it would be a change of meaning worth noticing.
	EXPECT_EQ(window_of(at_ns(RISK_CLOCK_WIDTH * 3), RISK_CLOCK_LOG2),
			  window_of(at_ns(0), RISK_CLOCK_LOG2) + 3);
}

TEST(RiskWindow, AWiderWindowSwallowsANarrowerOnesBoundary) {
	// The width really is 2^log2: an instant that starts a new 1024 ns window
	// is still inside the first 2048 ns one.
	EXPECT_NE(window_of(at_ns(RISK_CLOCK_WIDTH), RISK_CLOCK_LOG2),
			  window_of(at_ns(0), RISK_CLOCK_LOG2));
	EXPECT_EQ(window_of(at_ns(RISK_CLOCK_WIDTH), RISK_CLOCK_LOG2 + 1),
			  window_of(at_ns(0), RISK_CLOCK_LOG2 + 1));
}

TEST(RiskWindow, AZeroWidthWindowIsTheInstantItself) {
	// The degenerate shift, which a configuration could reach: every distinct
	// nanosecond is its own window, so nothing ever accumulates.
	EXPECT_NE(window_of(at_ns(0), 0), window_of(at_ns(1), 0));
	EXPECT_EQ(window_of(at_ns(7), 0), 7U);
}

TEST(RiskWindow, IsUsableAtCompileTime) {
	// constexpr because it sits inside rate_limiter::used, which is on the
	// per-command screening path. @see risk_management/window.hpp
	static_assert(window_of(monotonic_time{monotonic_clock::duration{2048}},
							RISK_CLOCK_LOG2) == 2U);
	SUCCEED();
}
