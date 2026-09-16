#pragma once
// The cache line, as three names.
//
// Every hot structure in this tree pads to a cache line to keep two threads'
// writes off one another, and each of them reached for
// `std::hardware_destructive_interference_size` directly. That is the right
// value and the wrong spelling to repeat, for a reason that shows up as a
// warning rather than as a bug - see below.
//
// The three constants answer three different questions and are not synonyms
// even where the numbers agree on this host:
//
//   CACHE_LINE_SIZE      how wide is one line
//   COLOCATION_SIZE      how much fits together so one reader pays one miss

#include <new>

namespace exchange::core::concurrency {

namespace detail {
/// @brief What the constants below fall back to where the standard library
/// does not offer @c <new>'s interference sizes.
///
/// libc++ only shipped @c __cpp_lib_hardware_interference_size in LLVM 19, so
/// an older Homebrew LLVM or AppleClang turns a header eleven files include
/// into a hard compile error. Folly guards the same way
/// (@c folly/lang/Align.h:148) and for the same reason.
///
/// @warning This deliberately mirrors what the compilers here *report* rather
///          than what the hardware *has*. 64 is what both GCC and clang answer
///          on x86-64 and on arm64, so a build with the guard and a build
///          without it lay out identically. Picking the truer 128 for Apple
///          silicon would make the layout depend on the standard library
///          version, which is the ODR-with-an-ABI-break that CACHE_LINE_SIZE's
///          own warning is about. Under-padding on that target is real and is
///          caught at run time instead - see
///          cache_line_size() in cache_probe.hpp and the invariant the
///          test beside it pins.
inline constexpr std::size_t ASSUMED_CACHE_LINE_SIZE = 64;
} // namespace detail

/**
 * @brief The width of one cache line.
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
 * suppressed per use
 *
 * @warning The ODR hazard is not removed by naming it, only localised. Every
 *          target here is built from one set of flags, so the value is the same
 *          across this tree; a consumer that compiles against these headers
 *          with a different @c destructive-interference-size still gets a
 *          different layout for anything padded with it. That is a property of
 *          the standard's facility, not of this constant.
 *
 * @warning It is also not reliably the hardware's line. The compiler reports 64
 *          on x86-64 and 64 on arm64, but Apple silicon runs a 128-byte line
 *          (@c hw.cachelinesize), so @c macos-arm64-* pads to half a line and
 *          gets no diagnostic. Padding that must be right rather than merely
 *          conventional wants @c FALSE_SHARING_RANGE, which is written down
 *          here rather than asked of the compiler.
 */
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t CACHE_LINE_SIZE =
	std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t CACHE_LINE_SIZE = detail::ASSUMED_CACHE_LINE_SIZE;
#endif

/**
 * @brief Bytes two objects can share while still being promised one cache line.
 *
 * The other half of the standard's pair, and a different question rather than a
 * synonym. @c CACHE_LINE_SIZE answers "how wide is a line"; this answers "how
 * much fits together so one reader pays one miss". A structure with a single
 * owner has no false sharing to avoid and wants this one - padding it to a
 * separation would buy nothing and cost table density.
 *
 * @note Usually the same number on x86-64 and Apple silicon, but not the same
 *       *guarantee*: the standard permits the constructive size to be smaller,
 *       and reaching for whichever happens to be equal today is how a layout
 *       argument stops being true on the next target. Name the one you mean.
 *       @see execution::detail::SLOT_STRIDE for the worked case.
 */
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t COLOCATION_SIZE =
	std::hardware_constructive_interference_size;
#else
inline constexpr std::size_t COLOCATION_SIZE = detail::ASSUMED_CACHE_LINE_SIZE;
#endif

/**
 * @brief Separation two concurrently-written locations need to stop contending.
 *
 * @par Why this is not a synonym for CACHE_LINE_SIZE
 * A line apart satisfies the coherence protocol and not necessarily the
 * machine. Intel's L2 spatial prefetcher moves lines in aligned 128-byte pairs,
 * so two atomics 64 bytes apart can still ride one pair between two cores'
 * L2. Folly assumes exactly that and hardcodes 128 on everything but ARM and
 * s390x (@c folly/lang/Align.h), citing microbenchmarked atomic increment on
 * Sandy Bridge; Intel's optimisation manual gives the same advice.
 *
 * @par Why it is nevertheless one line here
 * That effect is a property of a host, and assuming it doubles the footprint of
 * everything padded. @c BM_FalseSharing in
 * [benchmark/core/optimisation/false_sharing.bench.cpp](../../../benchmark/core/optimisation/false_sharing.bench.cpp)
 * hammers two atomics 8, 64, 128 and 256 bytes apart on two cores sharing an
 * LLC, with the 8-byte case as a control that must come out dramatically the
 * slowest or the run measured nothing. One line is what that says on the hosts
 * it has been run on. Where it says otherwise, this expression becomes
 * @c 2 * CACHE_LINE_SIZE and every structure padded with it moves together.
 *
 * @warning Deliberately not a build option, and not a macro. It decides the
 *          layout of every type padded with it, so a @c -D that two build trees
 *          set differently is precisely the ODR-with-an-ABI-break hazard
 *          @c CACHE_LINE_SIZE warns about above - reintroduced on purpose and
 *          still with no diagnostic. One value per source tree, edited in one
 *          place, rebuilt everywhere.
 *
 * @par When to use this rather than CACHE_LINE_SIZE
 * This one separates a *small, fixed* set of locations that different threads
 * write in a hot loop - a queue's two cursors, one record per thread, one row
 * per symbol. What keeps @c CACHE_LINE_SIZE is a structure whose *neighbours*
 * are walked: @c wait_free_hash_map probes linearly on collision, so its bucket
 * stride is a cost every lookup pays, against contention that is incidental
 * there rather than designed in.
 *
 * @note Folly's constant would not help even if copied: its 64-on-ARM is wrong
 *       on Apple silicon for the same reason the standard's is. The argument is
 *       the transferable part, not the number.
 */
inline constexpr std::size_t FALSE_SHARING_RANGE = CACHE_LINE_SIZE;

} // namespace exchange::core::concurrency
