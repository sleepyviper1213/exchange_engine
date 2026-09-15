// What the compiler was told about the cache line, checked against what the
// machine actually has.
//
// cache.hpp carries a @warning that the standard's interference sizes are
// whatever the toolchain was configured with, and that a layout built on them
// can be wrong with no diagnostic. This is the diagnostic. It is the only place
// in the suite where a passing run depends on the host rather than on the code,
// and that is deliberate: the failure it is built to catch is a *host* the
// constants are wrong for, not a regression anyone introduced.
//
// A failure here does not mean the padding code is broken. It means this target
// needs FALSE_SHARING_RANGE raised - see the message each assertion carries.

#include "core/concurrency/cache.hpp"
#include "core/concurrency/cache_probe.hpp"

#include <gtest/gtest.h>

#include <cstddef>

using namespace exchange::core::concurrency;

namespace {
// Reported once as its own case rather than folded into a message, so a CI log
// says what the host was even when every expectation passes.
constexpr std::size_t CACHE_ASSUMED_LINE = CACHE_LINE_SIZE;
} // namespace

TEST(CacheLineAssumptions, HostReportsAPlausibleLine) {
	const std::size_t reported = cache_line_size();
	EXPECT_GT(reported, 0U);
	EXPECT_EQ(reported & (reported - 1U), 0U)
		<< "a cache line of " << reported << " bytes is not a power of two, so "
		<< "either the probe is wrong or alignas cannot express it";
}

// The invariant that actually matters. Padding two concurrently-written
// locations FALSE_SHARING_RANGE apart only separates them if that distance is
// at least one hardware line; below it they share a line whatever the compiler
// believed, and the coherence traffic the padding exists to prevent happens
// anyway.
//
// This is the assertion that fails on Apple silicon today: both GCC and clang
// report 64 on arm64 while hw.cachelinesize is 128.
TEST(CacheLineAssumptions, SeparationCoversAHardwareLine) {
	const std::size_t reported = cache_line_size();
	EXPECT_GE(FALSE_SHARING_RANGE, reported)
		<< "FALSE_SHARING_RANGE is " << FALSE_SHARING_RANGE
		<< " bytes but this host's cache line is " << reported
		<< " bytes, so every structure padded with it shares a line with its "
		   "neighbour. Raise FALSE_SHARING_RANGE in core/concurrency/cache.hpp "
		   "for this target.";
}

// Weaker and separate on purpose: CACHE_LINE_SIZE disagreeing with the host is
// worth knowing about even where FALSE_SHARING_RANGE has been raised to cover
// it, because COLOCATION_SIZE and every "is this one line" argument still read
// the compiler's number.
TEST(CacheLineAssumptions, CompileTimeConstantMatchesTheHost) {
	const std::size_t reported = cache_line_size();
	EXPECT_EQ(CACHE_ASSUMED_LINE, reported)
		<< "the toolchain says a line is " << CACHE_ASSUMED_LINE
		<< " bytes and the OS says " << reported
		<< "; layout arguments written in terms of CACHE_LINE_SIZE do not hold "
		   "on this host";
}
