#pragma once
// The cache line, as one name.
//
// Every hot structure in this tree pads to a cache line to keep two threads'
// writes off one another, and each of them reached for
// `std::hardware_destructive_interference_size` directly. That is the right
// value and the wrong spelling to repeat, for a reason that shows up as a
// warning rather than as a bug - see below.

#include <cstddef>
#include <new>

namespace exchange::core::optimisation {

/**
 * @brief Bytes to pad or align to so two objects cannot share a cache line.
 *
 * @par Why this is a constant here rather than the standard's name at each use
 * GCC warns on every direct use of
 * @c std::hardware_destructive_interference_size (@c -Winterference-size), and
 * the warning is a real one: the value depends on @c --param
 * destructive-interference-size, so two translation units compiled with
 * different flags can disagree about it, and a type whose @c alignas is written
 * in terms of it then has two layouts and no diagnostic. That is an ODR
 * violation with an ABI break inside it.
 *
 * The project's answer is to accept the value and read it in one place. The
 * warning itself is off tree-wide - @c -Wno-interference-size in
 * [cmake/Warnings.cmake](../../../cmake/Warnings.cmake) - rather than
 * suppressed per use, which is what @c spsc_queue and @c fast_queue each used
 * to do with their own `#pragma GCC diagnostic` pair around their member
 * blocks while the other nine users had none and simply did not trip it.
 *
 * @warning The ODR hazard is not removed by naming it, only localised. Every
 *          target here is built from one set of flags, so the value is the same
 *          across this tree; a consumer that compiles against these headers
 * with a different @c destructive-interference-size still gets a different
 *          layout for anything padded with it. That is a property of the
 *          standard's facility, not of this constant.
 *
 * @note 64 on x86-64 and on Apple silicon, which is every platform this builds
 *       for. It is read rather than hardcoded so a platform where it is not 64
 *       (some POWER and older ARM parts run 128) is padded correctly instead of
 *       falsely sharing.
 */
inline constexpr std::size_t CACHE_LINE_SIZE =
	std::hardware_destructive_interference_size;

/**
 * @brief Bytes two objects can share while still being promised one cache line.
 *
 * The other half of the standard's pair, and a different question rather than a
 * synonym. @c CACHE_LINE_SIZE answers "how far apart must these be so two
 * writers never collide"; this answers "how much fits together so one reader
 * pays one miss". A structure with a single owner has no false sharing to avoid
 * and wants this one - padding it to the destructive size would buy nothing and
 * cost table density.
 *
 * @note Usually the same number on x86-64 and Apple silicon, but not the same
 *       *guarantee*: the standard permits the constructive size to be smaller,
 *       and reaching for whichever happens to be equal today is how a layout
 *       argument stops being true on the next target. Name the one you mean.
 *       @see execution::detail::SLOT_STRIDE for the worked case.
 */
inline constexpr std::size_t COLOCATION_SIZE =
	std::hardware_constructive_interference_size;

} // namespace exchange::core::optimisation
