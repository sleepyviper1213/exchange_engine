#include "core/simd/ladder.hpp"
#include "simd_ladder.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

using namespace exchange;
using namespace exchange::core;

namespace {

TEST(SimdLadderConsume, TakingNothingReachesNoLevel) {
	const auto sizes = simd_ladder_sizes(32, 1);
	for (const std::int64_t wanted : {0LL, -1LL}) {
		const auto taken =
			simd::consume(std::span<const std::int64_t>{sizes}, wanted);
		EXPECT_EQ(taken.levels, 0u);
		EXPECT_EQ(taken.filled, 0);
		EXPECT_TRUE(taken.is_complete()) << "wanted " << wanted;
	}
}

TEST(SimdLadderConsume, EmptyLadderFillsNothingAndReportsTheShortfall) {
	const auto taken = simd::consume(std::span<const std::int64_t>{}, 100);
	EXPECT_EQ(taken.levels, 0u);
	EXPECT_EQ(taken.filled, 0);
	EXPECT_EQ(taken.shortfall, 100);
	EXPECT_FALSE(taken.is_complete());
}

TEST(SimdLadderConsume, PartOfTheFirstLevelReachesOnlyThatLevel) {
	const std::vector<std::int64_t> sizes{10, 20, 30};
	const auto taken = simd::consume(std::span<const std::int64_t>{sizes}, 4);
	EXPECT_EQ(taken.levels, 1u);
	EXPECT_EQ(taken.filled, 4);
	EXPECT_TRUE(taken.is_complete());
}

// The boundary the blocked scan is most likely to get wrong: a size that ends
// exactly on a level. The level is consumed whole and the walk stops after it,
// so the count is 1 and not 2 - which is the difference between erasing the
// level that emptied and erasing the one behind it that did not.
TEST(SimdLadderConsume, SizeEndingExactlyOnALevelDoesNotReachTheNext) {
	const std::vector<std::int64_t> sizes{10, 20, 30};
	const auto taken = simd::consume(std::span<const std::int64_t>{sizes}, 10);
	EXPECT_EQ(taken.levels, 1u);
	EXPECT_EQ(taken.filled, 10);
	EXPECT_TRUE(taken.is_complete());
}

TEST(SimdLadderConsume, MoreThanTheLadderHoldsReachesEveryLevel) {
	const std::vector<std::int64_t> sizes{10, 20, 30};
	const auto taken = simd::consume(std::span<const std::int64_t>{sizes}, 100);
	EXPECT_EQ(taken.levels, 3u);
	EXPECT_EQ(taken.filled, 60);
	EXPECT_EQ(taken.shortfall, 40);
	EXPECT_FALSE(taken.is_complete());
}

// The exhaustive one, and the reason the others are allowed to be short. Every
// length crosses every register-boundary shape, and every cumulative sum (plus
// the values either side of it) is a size that ends on, just before, or just
// after a level - which is where a blocked scan goes wrong if it is going to.
//
// What a failure looks like: `levels` off by one for a `wanted` that equals
// some prefix sum, most likely a multiple of the vector width. If the kernel
// stopped skipping blocks and walked scalar this could not fail, which is why
// it also asserts that a non-trivial vector width was in play.
TEST(SimdLadderConsume, MatchesScalarAtEveryLengthAndEveryBoundary) {
	for (const std::size_t count : simd_ladder_lengths()) {
		const auto sizes = simd_ladder_sizes(count, count + 3);
		const auto span  = std::span<const std::int64_t>{sizes};

		for (const std::int64_t wanted :
			 simd_ladder_interesting_targets(span)) {
			EXPECT_EQ(simd::consume(span, wanted),
					  simd_ladder_reference_consume(span, wanted))
				<< "length " << count << ", wanted " << wanted;
		}
	}
}

TEST(SimdLadderConsume, InterleavedMatchesScalarAtEveryLengthAndBoundary) {
	for (const std::size_t count : simd_ladder_lengths()) {
		const auto sizes = simd_ladder_sizes(count, count + 5);
		const auto span  = std::span<const std::int64_t>{sizes};
		const auto pairs = simd_ladder_interleaved(span);

		for (const std::int64_t wanted :
			 simd_ladder_interesting_targets(span)) {
			EXPECT_EQ(simd::consume_interleaved(pairs, wanted),
					  simd_ladder_reference_consume(span, wanted))
				<< "length " << count << ", wanted " << wanted;
		}
	}
}

TEST(SimdLadderConsume, ThirtyTwoBitMatchesScalarAtEveryLengthAndBoundary) {
	for (const std::size_t count : simd_ladder_lengths()) {
		const auto wide  = simd_ladder_sizes(count, count + 13);
		const auto sizes = simd_ladder_narrowed(wide);

		for (const std::int64_t wanted :
			 simd_ladder_interesting_targets(wide)) {
			EXPECT_EQ(
				simd::consume(std::span<const std::int32_t>{sizes}, wanted),
				simd_ladder_reference_consume(wide, wanted))
				<< "length " << count << ", wanted " << wanted;
		}
	}
}

// A block of 32-bit sizes can exceed a 32-bit lane, so the blocked scan's
// running total has to be 64-bit too. With a wrapped block sum the comparison
// against what is left goes the wrong way and the walk stops at the wrong
// level - not merely reporting a wrong `filled`, but a wrong count of levels
// to erase.
TEST(SimdLadderConsume, ThirtyTwoBitBlockSumsDoNotWrap) {
	constexpr std::int32_t SIMD_CONSUME_BIG = 2'000'000'000;
	const std::vector<std::int32_t> sizes(32, SIMD_CONSUME_BIG);
	const std::int64_t wanted = std::int64_t{SIMD_CONSUME_BIG} * 20;

	const auto taken =
		simd::consume(std::span<const std::int32_t>{sizes}, wanted);
	EXPECT_EQ(taken.levels, 20u);
	EXPECT_EQ(taken.filled, wanted);
	EXPECT_TRUE(taken.is_complete());
}

} // namespace
