#include "core/metrics/percentile.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>

// The one contract `percentile` exists to enforce, checked where it fires.
//
// In its own file because a suite that forks should not share a binary with one
// that does not. @see testing.md
//
// These are preconditions in the CLAUDE.md sense rather than input validation:
// nothing in this tree derives a percentile from a message, a file or a flag,
// so the only way to reach them is to write a bad literal - which is a
// programming error and is caught at compile time wherever the value is
// constant-evaluated. What is left to test at run time is the case the compiler
// cannot see, a percentile built from a variable.
//
// They matter because `rank_of` no longer defends itself. It used to clamp, and
// dropping that was only sound because this holds: a negative double cast to
// `std::size_t` is undefined behaviour, not a large number.

using exchange::core::metrics::percentile;
using testing::HasSubstr;

namespace {

/// Laundered through a function so the compiler cannot constant-fold the
/// construction into a compile error and leave the case with nothing to run.
[[nodiscard]] percentile percentile_from(double q) { return percentile{q}; }

} // namespace

TEST(PercentileDeath, ANegativeQuantileIsRefused) {
	// The dangerous one. Cast to std::size_t it is undefined behaviour rather
	// than a large number, so `rank_of` could not have clamped its way out of
	// it even if it still tried.
	EXPECT_DEATH((void)percentile_from(-0.01), HasSubstr("quantile in [0,1]"));
}

TEST(PercentileDeath, AQuantileAboveOneIsRefused) {
	// The likelier typo: 99 for the 99th percentile, where the quantile wanted
	// is 0.99. Both are plausible-looking arguments to something called
	// `quantile`, which is the whole reason this is a type.
	EXPECT_DEATH((void)percentile_from(99.0), HasSubstr("quantile in [0,1]"));
	EXPECT_DEATH((void)percentile_from(1.01), HasSubstr("quantile in [0,1]"));
}

TEST(PercentileDeath, ANaNIsRefusedByItsOwnCheck) {
	// Separately from the range check, and it has to be: every comparison
	// against a NaN is false, so `q >= 0.0 && q <= 1.0` would let one straight
	// through. It is caught by `q == q` instead.
	EXPECT_DEATH((void)percentile_from(
					 std::numeric_limits<double>::quiet_NaN()),
				 HasSubstr("cannot be NaN"));
}

TEST(PercentileDeath, TheEndsOfTheRangeAreNotRefused) {
	// The boundary, from the legal side - so the assertions above are testing
	// the bound rather than the fact that they fire at all.
	EXPECT_EQ(percentile_from(0.0).value(), 0.0);
	EXPECT_EQ(percentile_from(1.0).value(), 1.0);
}
