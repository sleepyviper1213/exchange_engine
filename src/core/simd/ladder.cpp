// The ladder kernels, compiled once per instruction set.
//
// `foreach_target.h` re-includes this file once for every target in
// `HWY_TARGETS` - SSE2, SSE4, AVX2, AVX3 and so on, or NEON and SVE on arm64 -
// and `HWY_NAMESPACE` expands to a different inner namespace each time, so the
// bodies below are written once and exist several times over. The `HWY_ONCE`
// section at the bottom is compiled in the final pass only; that is where the
// dispatch tables live and where the public overloads resolve themselves to
// whichever body the CPU actually supports.
//
// Two consequences worth knowing before editing this file. It must not be
// swept into a unity batch - `core/CMakeLists.txt` marks it
// `SKIP_UNITY_BUILD_INCLUSION` - because the re-inclusion machinery needs the
// file to be a translation unit of its own. And nothing here may be given an
// architecture flag on the command line: Highway attaches the per-target
// attributes itself, and a global `-mavx2` would tell the SSE2 body it may
// emit AVX2, which is exactly the fault the run-time check exists to prevent.

#include "core/simd/ladder.hpp"

#include <cstddef>
#include <cstdint>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "core/simd/ladder.cpp"
#include "hwy/foreach_target.h" // IWYU pragma: keep
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace exchange::core::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/// @brief Independent accumulator chains kept in flight by the summing
///        kernels.
///
/// One accumulator is latency-bound rather than throughput-bound: every
/// iteration's add depends on the previous one's result, so the loop runs at
/// the add's latency however many loads the machine could have issued. Four
/// chains break that dependency and let the out-of-order engine keep the
/// adders fed.
///
/// Measured (MinGW GCC 16, -O3 + LTO, i7-3770K, SSE4, 4096 levels): the
/// single-accumulator form took 1103 ns where the plain scalar loop - which
/// GCC auto-vectorises *and* unrolls by itself - took 625 ns. Four
/// accumulators are what make the kernel worth dispatching to rather than a
/// slower way of reaching the same instructions.
constexpr std::size_t UNROLL = 4;

/// @brief Sum @p count elements, reading them through @p load a register at a
///        time and through @p at one at a time for the tail.
///
/// The three summing kernels differ only in how a register is filled -
/// straight load, widening load, de-interleaving load - so the loop shape
/// lives here once and each of them supplies its own two accessors.
template <class D, class LoadLanes, class ScalarAt>
hn::TFromD<D> accumulate(D tag, std::size_t count, LoadLanes load,
						 ScalarAt at) {
	const std::size_t lanes = hn::Lanes(tag);
	const std::size_t step  = lanes * UNROLL;

	auto first  = hn::Zero(tag);
	auto second = hn::Zero(tag);
	auto third  = hn::Zero(tag);
	auto fourth = hn::Zero(tag);

	std::size_t index = 0;
	for (; index + step <= count; index += step) {
		first  = hn::Add(first, load(index));
		second = hn::Add(second, load(index + lanes));
		third  = hn::Add(third, load(index + (2 * lanes)));
		fourth = hn::Add(fourth, load(index + (3 * lanes)));
	}

	auto accumulator = hn::Add(hn::Add(first, second), hn::Add(third, fourth));
	for (; index + lanes <= count; index += lanes)
		accumulator = hn::Add(accumulator, load(index));

	auto sum = hn::ReduceSum(tag, accumulator);
	for (; index < count; ++index) sum += at(index);
	return sum;
}

/// @brief Sum of @p count 64-bit sizes at @p sizes.
///
/// The straight load, which is all this one has to supply. @see accumulate for
/// the loop, the unroll and why it is four wide.
std::int64_t total_i64(const std::int64_t *sizes, std::size_t count) {
	const hn::ScalableTag<std::int64_t> tag;
	return accumulate(
		tag,
		count,
		[tag, sizes](std::size_t index) { return hn::LoadU(tag, sizes + index); },
		[sizes](std::size_t index) { return sizes[index]; });
}

/// @brief Sum of @p count 32-bit sizes at @p sizes, accumulated in 64 bits.
///
/// `Rebind` names a tag with the same lane *count* as the 64-bit one and half
/// the lane width, so `LoadU` reads exactly as many 32-bit sizes as the wide
/// register has room for and `PromoteTo` widens them in one instruction
/// (`vpmovsxdq` and its equivalents). Half the elements per iteration of the
/// 64-bit kernel, and no lane that can wrap. @see ladder.hpp on why the sum is
/// wider than the input.
std::int64_t total_i32(const std::int32_t *sizes, std::size_t count) {
	const hn::ScalableTag<std::int64_t> wide;
	const hn::Rebind<std::int32_t, decltype(wide)> narrow;
	return accumulate(
		wide,
		count,
		[wide, narrow, sizes](std::size_t index) {
			return hn::PromoteTo(wide, hn::LoadU(narrow, sizes + index));
		},
		[sizes](std::size_t index) { return sizes[index]; });
}

/// @brief Finish a walk that the vector loop has narrowed to @p sizes, with
///        @p left still wanted and @p result holding what the whole blocks
///        already contributed.
///
/// Shared by both element widths because it is the part that cannot be
/// vectorised: it runs inside the single block where the size runs out, and
/// over the tail the register width did not cover. At most `2 * lanes - 1`
/// levels, once per call.
template <class T>
void finish(consumption &result, const T *sizes, std::size_t index,
			std::size_t count, std::int64_t &left) {
	for (; index < count && left > 0; ++index) {
		const auto size  = static_cast<std::int64_t>(sizes[index]);
		const auto taken = size < left ? size : left;
		left -= taken;
		result.filled += taken;
		++result.levels;
	}
}

/// @brief Walk 64-bit @p sizes until @p wanted is exhausted.
///
/// The loop skips a whole register of levels at a time: one load, one fold,
/// one comparison for `lanes` levels, where the scalar walk tests - and
/// mispredicts - once per level. Only the block that the size runs out inside
/// is examined level by level, and `finish` does that.
///
/// The comparison is `>=` rather than `>` deliberately. A block that exactly
/// equals what is left must fall through to `finish`, because the last level
/// of it is consumed whole and the walk stops *after* it, not before - and
/// `finish` stopping on `left > 0` is what gets that boundary right.
consumption consume_i64(const std::int64_t *sizes, std::size_t count,
						std::int64_t wanted) {
	consumption result{};
	if (wanted <= 0) return result;

	const hn::ScalableTag<std::int64_t> tag;
	const std::size_t lanes = hn::Lanes(tag);

	std::int64_t left = wanted;
	std::size_t index = 0;
	for (; index + lanes <= count; index += lanes) {
		const std::int64_t block =
			hn::ReduceSum(tag, hn::LoadU(tag, sizes + index));
		if (block >= left) break;
		left -= block;
		result.filled += block;
		result.levels += lanes;
	}

	finish(result, sizes, index, count, left);
	result.shortfall = left;
	return result;
}

/// @brief Walk 32-bit @p sizes until @p wanted is exhausted.
/// @copydetails consume_i64
/// @note The block sum is taken in 64-bit lanes for the reason @c total_i32
///       gives - a block of 32-bit sizes can exceed a 32-bit lane.
consumption consume_i32(const std::int32_t *sizes, std::size_t count,
						std::int64_t wanted) {
	consumption result{};
	if (wanted <= 0) return result;

	const hn::ScalableTag<std::int64_t> wide;
	const hn::Rebind<std::int32_t, decltype(wide)> narrow;
	const std::size_t lanes = hn::Lanes(wide);

	std::int64_t left = wanted;
	std::size_t index = 0;
	for (; index + lanes <= count; index += lanes) {
		const std::int64_t block = hn::ReduceSum(
			wide, hn::PromoteTo(wide, hn::LoadU(narrow, sizes + index)));
		if (block >= left) break;
		left -= block;
		result.filled += block;
		result.levels += lanes;
	}

	finish(result, sizes, index, count, left);
	result.shortfall = left;
	return result;
}

/// @brief Sum of the size halves of @p levels {key, size} pairs at @p pairs.
///
/// `LoadInterleaved2` reads `2 * lanes` elements and splits them into the two
/// registers by position: keys in one, sizes in the other. One `vld2q` on
/// NEON, a shuffle pair on x86 - and either way the price lanes are dropped
/// without a gather, which is what lets an array-of-structs ladder keep its
/// layout. @see ladder.hpp on why both shapes exist.
std::int64_t total_interleaved_i64(const std::int64_t *pairs,
								   std::size_t levels) {
	const hn::ScalableTag<std::int64_t> tag;
	return accumulate(
		tag,
		levels,
		[tag, pairs](std::size_t index) {
			hn::VFromD<decltype(tag)> keys;
			hn::VFromD<decltype(tag)> sizes;
			hn::LoadInterleaved2(tag, pairs + (2 * index), keys, sizes);
			return sizes;
		},
		[pairs](std::size_t index) { return pairs[(2 * index) + 1]; });
}

/// @brief Finish an interleaved walk. @copydetails finish
void finish_interleaved(consumption &result, const std::int64_t *pairs,
						std::size_t index, std::size_t levels,
						std::int64_t &left) {
	for (; index < levels && left > 0; ++index) {
		const std::int64_t size = pairs[(2 * index) + 1];
		const std::int64_t taken = size < left ? size : left;
		left -= taken;
		result.filled += taken;
		++result.levels;
	}
}

/// @brief Walk {key, size} pairs until @p wanted is exhausted.
/// @copydetails consume_i64
consumption consume_interleaved_i64(const std::int64_t *pairs,
									std::size_t levels, std::int64_t wanted) {
	consumption result{};
	if (wanted <= 0) return result;

	const hn::ScalableTag<std::int64_t> tag;
	const std::size_t lanes = hn::Lanes(tag);

	std::int64_t left = wanted;
	std::size_t index = 0;
	for (; index + lanes <= levels; index += lanes) {
		hn::VFromD<decltype(tag)> keys;
		hn::VFromD<decltype(tag)> sizes;
		hn::LoadInterleaved2(tag, pairs + (2 * index), keys, sizes);

		const std::int64_t block = hn::ReduceSum(tag, sizes);
		if (block >= left) break;
		left -= block;
		result.filled += block;
		result.levels += lanes;
	}

	finish_interleaved(result, pairs, index, levels, left);
	result.shortfall = left;
	return result;
}

} // namespace exchange::core::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace exchange::core::simd {

HWY_EXPORT(total_i64);
HWY_EXPORT(total_i32);
HWY_EXPORT(consume_i64);
HWY_EXPORT(consume_i32);
HWY_EXPORT(total_interleaved_i64);
HWY_EXPORT(consume_interleaved_i64);

std::int64_t total(std::span<const std::int64_t> sizes) noexcept {
	return HWY_DYNAMIC_DISPATCH(total_i64)(sizes.data(), sizes.size());
}

std::int64_t total(std::span<const std::int32_t> sizes) noexcept {
	return HWY_DYNAMIC_DISPATCH(total_i32)(sizes.data(), sizes.size());
}

consumption consume(std::span<const std::int64_t> sizes,
					std::int64_t wanted) noexcept {
	return HWY_DYNAMIC_DISPATCH(consume_i64)(sizes.data(),
											 sizes.size(),
											 wanted);
}

consumption consume(std::span<const std::int32_t> sizes,
					std::int64_t wanted) noexcept {
	return HWY_DYNAMIC_DISPATCH(consume_i32)(sizes.data(),
											 sizes.size(),
											 wanted);
}

std::int64_t total_interleaved(std::span<const std::int64_t> pairs) noexcept {
	return HWY_DYNAMIC_DISPATCH(total_interleaved_i64)(pairs.data(),
													   pairs.size() / 2);
}

consumption consume_interleaved(std::span<const std::int64_t> pairs,
								std::int64_t wanted) noexcept {
	return HWY_DYNAMIC_DISPATCH(consume_interleaved_i64)(pairs.data(),
														 pairs.size() / 2,
														 wanted);
}

} // namespace exchange::core::simd

#endif // HWY_ONCE
