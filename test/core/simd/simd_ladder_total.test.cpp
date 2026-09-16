#include "simd_ladder.fixture.hpp"

#include "core/simd/ladder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace exchange;
using namespace exchange::core;

namespace {

TEST(SimdLadderTotal, EmptyLadderSumsToZero) {
	EXPECT_EQ(simd::total(std::span<const std::int64_t>{}), 0);
	EXPECT_EQ(simd::total(std::span<const std::int32_t>{}), 0);
	EXPECT_EQ(simd::total_interleaved(std::span<const std::int64_t>{}), 0);
}

// Every length from empty to several registers, so the fold and the scalar
// tail are both exercised at every offset rather than at whichever one this
// machine's vector width happens to produce.
TEST(SimdLadderTotal, MatchesScalarAtEveryLength) {
	for (const std::size_t count : simd_ladder_lengths()) {
		const auto sizes = simd_ladder_sizes(count, count + 1);
		EXPECT_EQ(simd::total(std::span<const std::int64_t>{sizes}),
				  simd_ladder_reference_total(sizes))
			<< "length " << count;
	}
}

TEST(SimdLadderTotal, InterleavedIgnoresThePriceHalf) {
	for (const std::size_t count : simd_ladder_lengths()) {
		const auto sizes = simd_ladder_sizes(count, count + 7);
		const auto pairs = simd_ladder_interleaved(sizes);
		EXPECT_EQ(simd::total_interleaved(pairs),
				  simd_ladder_reference_total(sizes))
			<< "length " << count;
	}
}

// A price large enough to dominate the sum if a lane ever picked one up. The
// de-interleave dropping the wrong half would be invisible against the small
// prices the fixture generates, and very visible here.
TEST(SimdLadderTotal, InterleavedIsNotFooledByHugePrices) {
	std::vector<std::int64_t> pairs;
	constexpr std::int64_t SIMD_TOTAL_HUGE_PRICE = 1'000'000'000'000;
	for (int level = 0; level < 37; ++level) {
		pairs.push_back(SIMD_TOTAL_HUGE_PRICE + level);
		pairs.push_back(7);
	}
	EXPECT_EQ(simd::total_interleaved(pairs), 37 * 7);
}

// The reason the 32-bit overload widens on load. Two levels of INT32_MAX
// already overflow an int32 lane, and a lane accumulates count/width of them -
// so summing in 32-bit lanes is wrong from the second full register onwards,
// not at some implausible depth. This is the test that fails if the PromoteTo
// is ever "optimised" away.
TEST(SimdLadderTotal, ThirtyTwoBitSizesAccumulateInSixtyFourBits) {
	constexpr std::size_t SIMD_TOTAL_WIDE_LEVELS = 64;
	const std::vector<std::int32_t> sizes(SIMD_TOTAL_WIDE_LEVELS,
										  std::numeric_limits<std::int32_t>::max());

	const std::int64_t expected =
		static_cast<std::int64_t>(SIMD_TOTAL_WIDE_LEVELS) *
		std::numeric_limits<std::int32_t>::max();

	EXPECT_EQ(simd::total(std::span<const std::int32_t>{sizes}), expected);
	EXPECT_GT(expected, std::numeric_limits<std::int32_t>::max());
}

TEST(SimdLadderTotal, ThirtyTwoBitMatchesScalarAtEveryLength) {
	for (const std::size_t count : simd_ladder_lengths()) {
		const auto wide  = simd_ladder_sizes(count, count + 11);
		const auto sizes = simd_ladder_narrowed(wide);
		EXPECT_EQ(simd::total(std::span<const std::int32_t>{sizes}),
				  simd_ladder_reference_total(wide))
			<< "length " << count;
	}
}

} // namespace
