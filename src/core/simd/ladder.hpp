#pragma once
// The two questions a price ladder gets asked about many levels at once, and
// their vector answers.
//
// A ladder - `market_data::l2_book`'s sides - is a contiguous, price-sorted
// run of levels. Two reads walk the whole of it:
//
//   total()    how much rests across a range of levels
//   consume()  how many levels a given size clears, and how much it actually
//              gets - an aggressive order's reach, and therefore the count of
//              levels a caller may then clear in one operation
//
// Both are trivially parallel in the data, and both are written scalar as a
// dependent chain: `sum += level.size` once per level, with - in consume's
// case - an unpredictable `if (left <= 0) break;` in the middle of it. The
// vector forms accumulate a register's worth of levels per iteration and fold
// once, so `width` interior branches become one.
//

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::core::simd {

/**
 * @brief Total size resting across @p sizes.
 *
 * @param sizes A contiguous run of per-level sizes, in any order - addition
 *        does not care, and the ladder's sort is irrelevant to this read.
 * @return The sum, always 64-bit.
 *
 * @par Why the answer is wider than the input
 * A sum crosses levels while an element does not, which is the same
 * distinction `quantity_t` and `volume_t` draw in the engine. The 32-bit
 * overload therefore widens on load rather than accumulating in 32-bit lanes:
 * two levels of `INT32_MAX` already overflow an `int32` lane, and a lane is a
 * partial sum over `size() / width` levels. It costs half the lanes per
 * register and buys an answer that is right.
 */
[[nodiscard]] CORE_EXPORT std::int64_t
total(std::span<const std::int64_t> sizes) noexcept;

/// @copydoc total(std::span<const std::int64_t>)
[[nodiscard]] CORE_EXPORT std::int64_t
total(std::span<const std::int32_t> sizes) noexcept;

/**
 * @brief Total size resting across an array of {key, size} pairs.
 *
 * @param pairs @c 2*n elements: each level's sort key at an even index and its
 *        size at the odd index after it. The keys are read and discarded - a
 *        de-interleaving load has to bring them into a register either way -
 *        so a caller need not separate them first.
 * @return The sum of the odd-indexed elements.
 *
 * @note @c pairs.size() being odd is a caller bug; the trailing half-level is
 *       ignored rather than diagnosed, because this runs on the read path and
 *       the shape is a property of the caller's type, not of its data.
 */
[[nodiscard]] CORE_EXPORT std::int64_t
total_interleaved(std::span<const std::int64_t> pairs) noexcept;

/**
 * @brief How far into a ladder a given size reaches.
 *
 * The decomposition of an aggressive order's walk. It is one call rather than
 * a loop at every call site because the two ways of writing that loop wrong -
 * counting a level when the size ran out exactly at its boundary, and counting
 * a partial last level whole - are both easy and both silent.
 */
struct consumption {
	/// @brief Levels reached, whole or in part. The count a caller may then
	///        erase in bulk; @c shortfall and the last level's own size say
	///        whether the last of them was emptied or merely reduced.
	std::size_t levels = 0;

	/// @brief Size actually taken, which is the size asked for unless the
	///        ladder ran out of levels first.
	std::int64_t filled = 0;

	/// @brief Size still wanted when the ladder ran out. Zero for a sweep the
	///        ladder could satisfy.
	std::int64_t shortfall = 0;

	/// @brief Did the ladder hold the whole size?
	[[nodiscard]] constexpr bool is_complete() const noexcept {
		return shortfall == 0;
	}

	bool operator==(const consumption &) const noexcept = default;
};

/**
 * @brief Walk @p sizes from the front until @p wanted is exhausted.
 *
 * @param sizes A contiguous run of per-level sizes in the order an aggressor
 *        meets them - best price first. Every entry must be positive: a ladder
 *        stores no empty level (a zero size removes the price), and a zero
 *        here would be counted as a level reached while contributing nothing.
 * @param wanted The size to take. Non-positive returns an empty result rather
 *        than an error - "take nothing" has an answer, and it is no levels.
 *
 * @par The bulk erase this exists to enable
 * @c consumption::levels is the count to clear in one operation. Clearing
 * levels one at a time through a per-price update is not merely slower by a
 * constant: on an array-backed ladder each removal shifts the tail, so
 * clearing @c k levels costs @c k shifts of the whole side where one bulk
 * erase costs one. The scan being branchless is the smaller half of the win.
 */
[[nodiscard]] CORE_EXPORT consumption
consume(std::span<const std::int64_t> sizes, std::int64_t wanted) noexcept;

/// @copydoc consume(std::span<const std::int64_t>, std::int64_t)
[[nodiscard]] CORE_EXPORT consumption
consume(std::span<const std::int32_t> sizes, std::int64_t wanted) noexcept;

/// @brief Walk an array of {key, size} pairs until @p wanted is exhausted.
/// @copydetails total_interleaved
/// @see consume(std::span<const std::int64_t>, std::int64_t) for everything
///      the two share, which is everything but where the sizes are.
[[nodiscard]] CORE_EXPORT consumption
consume_interleaved(std::span<const std::int64_t> pairs,
					std::int64_t wanted) noexcept;

} // namespace exchange::core::simd
