#pragma once
// Helpers shared by the ladder kernel suites: a scalar oracle, and ladders to
// point it at.
//
// The oracle is the whole point. These kernels skip a register of levels at a
// time and only descend to level-by-level inside the block where the size runs
// out, so the interesting failures are all off-by-one at a boundary - a block
// that exactly equals what is left, a size that lands on the last lane of a
// register, a tail shorter than the register. Asserting hand-written expected
// values would cover the cases somebody thought of; comparing against the
// obvious scalar walk over every length from empty to several registers covers
// the ones nobody did.

#include "core/simd/ladder.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

/// @brief The obvious scalar sum, which is what @c core::simd::total must
///        agree with.
[[nodiscard]] inline std::int64_t
simd_ladder_reference_total(std::span<const std::int64_t> sizes) noexcept {
	std::int64_t sum = 0;
	for (const std::int64_t size : sizes) sum += size;
	return sum;
}

/// @brief The obvious scalar walk, which is what @c core::simd::consume must
///        agree with. Deliberately written as the loop a call site would write
///        by hand rather than as a translation of the kernel.
[[nodiscard]] inline exchange::core::simd::consumption
simd_ladder_reference_consume(std::span<const std::int64_t> sizes,
							  std::int64_t wanted) noexcept {
	exchange::core::simd::consumption result{};
	if (wanted <= 0) return result;

	std::int64_t left = wanted;
	for (const std::int64_t size : sizes) {
		if (left <= 0) break;
		const std::int64_t taken = size < left ? size : left;
		left -= taken;
		result.filled += taken;
		++result.levels;
	}
	result.shortfall = left;
	return result;
}

/// @brief A ladder of @p count strictly positive sizes - the invariant the
///        kernels are allowed to assume.
[[nodiscard]] inline std::vector<std::int64_t>
simd_ladder_sizes(std::size_t count, std::uint64_t seed,
				  std::int64_t upper = 1'000) {
	std::mt19937_64 engine{seed};
	std::uniform_int_distribution<std::int64_t> size{1, upper};

	std::vector<std::int64_t> sizes(count);
	for (std::int64_t &entry : sizes) entry = size(engine);
	return sizes;
}

/// @brief The same ladder narrowed to 32-bit sizes, for the overloads that
///        take them.
///
/// An explicit cast per element rather than a @c vector range constructor: the
/// iterator-pair form narrows implicitly, which MSVC diagnoses as C4244 and
/// this tree promotes to an error. The values the generator produces fit a
/// 32-bit size comfortably; the ones that would not are built by hand in the
/// suites that test for wrapping.
[[nodiscard]] inline std::vector<std::int32_t>
simd_ladder_narrowed(std::span<const std::int64_t> sizes) {
	std::vector<std::int32_t> narrow;
	narrow.reserve(sizes.size());
	for (const std::int64_t entry : sizes)
		narrow.push_back(static_cast<std::int32_t>(entry));
	return narrow;
}

/// @brief The same ladder as {price, size} pairs, which is the shape
///        @c l2_book stores and the @c _interleaved kernels read.
[[nodiscard]] inline std::vector<std::int64_t>
simd_ladder_interleaved(std::span<const std::int64_t> sizes) {
	std::vector<std::int64_t> pairs;
	pairs.reserve(sizes.size() * 2);
	for (std::size_t index = 0; index < sizes.size(); ++index) {
		pairs.push_back(static_cast<std::int64_t>(index) + 1); // a price
		pairs.push_back(sizes[index]);
	}
	return pairs;
}

/// @brief Every cumulative sum of @p sizes, plus the boundaries either side of
///        each - the sizes a sweep is most likely to be wrong about.
[[nodiscard]] inline std::vector<std::int64_t>
simd_ladder_interesting_targets(std::span<const std::int64_t> sizes) {
	std::vector<std::int64_t> targets{-1, 0, 1};
	std::int64_t running = 0;
	for (const std::int64_t size : sizes) {
		running += size;
		targets.push_back(running - 1);
		targets.push_back(running);
		targets.push_back(running + 1);
	}
	targets.push_back(running * 2); // more than the ladder holds
	return targets;
}

/// @brief Lengths that cross every register boundary and tail shape, for a
///        vector width no test is allowed to assume.
[[nodiscard]] inline std::vector<std::size_t> simd_ladder_lengths() {
	std::vector<std::size_t> lengths;
	for (std::size_t count = 0; count <= 40; ++count) lengths.push_back(count);
	lengths.insert(lengths.end(), {63, 64, 65, 127, 128, 129, 257});
	return lengths;
}
