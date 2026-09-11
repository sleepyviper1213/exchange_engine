#include "core/metrics/percentile.hpp"
#include "core/metrics/quantile.hpp"

#include "core/metrics/histogram.hpp"

#include <gtest/gtest.h>

#include <compare>
#include <cstdint>
#include <span>
#include <vector>

// One definition of "p99", pinned.
//
// This file exists because there were three. Two agreed and the third took rank
// `floor(q * (n - 1))` where the others took `floor(q * n)` - so at n=100 a p99
// from the benchmark harness was the 99th sample and a p99 from the engine's
// histogram was the 100th. Nothing at either call site showed it. What is
// asserted below is therefore not that the rule is the best estimator, but that
// there is exactly one of it and that both lookups reach the same sample.

using exchange::core::metrics::histogram;
using exchange::core::metrics::percentile;
using exchange::core::metrics::quantile_of;
using exchange::core::metrics::rank_of;

// Spelled `percentile::P99` at each use rather than aliased to a local `P99`.
// `order_test` is one binary and a unity batch merges file scopes, so a
// bare `P99` at namespace scope is precisely the collision testing.md warns
// about - and these five are among the likeliest names in the tree to be
// wanted again by somebody else.

namespace {

/// 1..100, ascending - so the value at rank r is r + 1 and an off-by-one in the
/// rank is legible as an off-by-one in the answer.
[[nodiscard]] std::vector<std::uint64_t> quantile_ladder() {
	std::vector<std::uint64_t> values;
	values.reserve(100);
	for (std::uint64_t i = 1; i <= 100; ++i) values.push_back(i);
	return values;
}

} // namespace

TEST(Quantile, TheRankIsFloorOfQTimesCountClampedIntoRange) {
	// The rule, stated. n times q, truncated - not n-1, which is the variant
	// this file was written to eliminate.
	EXPECT_EQ(rank_of(percentile::P50, 100), 50U);
	EXPECT_EQ(rank_of(percentile::P99, 100), 99U);
	EXPECT_EQ(rank_of(percentile::P95, 100), 95U);
	// q * n lands exactly on n at the top, which is one past the end.
	EXPECT_EQ(rank_of(percentile::PMAX, 100), 99U);
}

TEST(Quantile, TheEndsOfTheRangeAreRanksRatherThanSpecialCases) {
	// There is no out-of-range case left to test here, and that is the point of
	// `percentile` existing: the check that used to live in this function now
	// lives in the constructor, so a bad value cannot reach `rank_of` at all.
	// What remains is arithmetic at the two ends. @see PercentileDeath
	EXPECT_EQ(rank_of(percentile{0.0}, 100), 0U);
	EXPECT_EQ(rank_of(percentile::PMAX, 100), 99U);
	EXPECT_EQ(rank_of(percentile{0.0}, 1), 0U);
	EXPECT_EQ(rank_of(percentile::PMAX, 1), 0U);
}

TEST(Quantile, NoSamplesMeansRankZeroAndAValueInitialisedAnswer) {
	// Not an error: a run that recorded nothing has no tail. The rank is only
	// safe to index when there is something to index, which is what
	// quantile_of checks for its caller.
	EXPECT_EQ(rank_of(percentile::P99, 0), 0U);
	const std::vector<std::uint64_t> none;
	EXPECT_EQ(quantile_of<std::uint64_t>(none, percentile::P99), 0U);
}

TEST(Quantile, ASortedRangeIsIndexedAtThatRank) {
	const auto values = quantile_ladder();
	// Rank 50 of 1..100 is the value 51. Stated as the arithmetic rather than
	// as a number, so a change to the rule fails here first.
	EXPECT_EQ(quantile_of<std::uint64_t>(values, percentile::P50), 51U);
	EXPECT_EQ(quantile_of<std::uint64_t>(values, percentile::P99), 100U);
	EXPECT_EQ(quantile_of<std::uint64_t>(values, percentile::PMAX), 100U);
	EXPECT_EQ(quantile_of<std::uint64_t>(values, percentile{0.0}), 1U);
}

TEST(Quantile, ASingleSampleIsEveryQuantileOfItself) {
	const std::vector<std::uint64_t> one{7};
	EXPECT_EQ(quantile_of<std::uint64_t>(one, percentile{0.0}), 7U);
	EXPECT_EQ(quantile_of<std::uint64_t>(one, percentile::P50), 7U);
	EXPECT_EQ(quantile_of<std::uint64_t>(one, percentile::PMAX), 7U);
}

TEST(Quantile, TheHistogramReachesTheSampleTheSortedRangeWould) {
	// The claim the whole file is for: two lookups, one rank. The histogram
	// walks cumulative bucket counts where a vector indexes directly, and they
	// have to land on the same sample - modulo the histogram's own bucketing,
	// which reports a bucket's upper bound rather than the value.
	histogram tail;
	std::vector<std::uint64_t> values;
	// 128 samples spanning several buckets, so the answer is not trivially the
	// only bucket that has anything in it.
	for (std::uint64_t i = 0; i < 128; ++i) {
		const std::uint64_t sample = i + 1;
		tail.record(sample);
		values.push_back(sample);
	}

	const histogram::snapshot snap = tail.read();
	for (const percentile q : {percentile{0.0},
							   percentile::P50,
							   percentile::P95,
							   percentile::P99,
							   percentile::P999,
							   percentile::PMAX}) {
		const std::uint64_t exact = quantile_of<std::uint64_t>(values, q);
		const std::uint64_t bucketed = snap.quantile(q);
		// The bucket the exact sample falls in is the one the histogram names,
		// so its upper bound is at least the sample and less than twice it -
		// buckets here are powers of two. @see histogram::upper_bound
		EXPECT_GE(bucketed, exact) << "q=" << q.value();
		EXPECT_LT(bucketed, exact * 2 + 2) << "q=" << q.value();
	}
}

TEST(Quantile, TheNamedPercentilesAreTheNumbersTheyClaim) {
	// Cheap, and the reason the constants exist: 0.99 and 0.999 differ by one
	// character and by two orders of magnitude of tail, and a typo between them
	// produces a figure that looks entirely reasonable.
	EXPECT_DOUBLE_EQ(percentile::P50.value(), 0.50);
	EXPECT_DOUBLE_EQ(percentile::P95.value(), 0.95);
	EXPECT_DOUBLE_EQ(percentile::P99.value(), 0.99);
	EXPECT_DOUBLE_EQ(percentile::P999.value(), 0.999);
	EXPECT_DOUBLE_EQ(percentile::PMAX.value(), 1.0);
}

TEST(Quantile, PercentilesOrderByHowFarOutTheTailIs) {
	// A total order, which a defaulted `<=>` over a double could not give:
	// comparison between doubles is partial only because either might be a NaN,
	// and the constructor has excluded that.
	static_assert(percentile::P50 < percentile::P95);
	static_assert(percentile::P95 < percentile::P99);
	static_assert(percentile::P99 < percentile::P999);
	static_assert(percentile::P999 < percentile::PMAX);
	// Negative zero is a legal way to spell the bottom of the range, and has to
	// compare equal to the other one - `std::strong_order` would say otherwise.
	static_assert(percentile{-0.0} == percentile{0.0});
	static_assert((percentile{-0.0} <=> percentile{0.0}) ==
				  std::strong_ordering::equal);
	SUCCEED() << "the assertions above are compile-time";
}
