#pragma once
// Which instruction set the ladder kernels actually resolved to, at run time.
//
// Worth exporting because the alternative is a green run that proves nothing.
// A build whose vector code never ran - because the dispatch fell back to SSE2,
// because a target was compiled out, because the CPU is older than the
// developer's - produces exactly the same test output and a benchmark that
// merely looks unremarkable. These two reads are how a test names the path it
// exercised and how a benchmark labels the number it produced.

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <cstddef>
#include <string_view>

namespace exchange::core::simd {

/**
 * @brief Name of the instruction set @c ladder.hpp's kernels dispatch to on
 *        this machine - @c "AVX3", @c "AVX2", @c "SSE2", @c "NEON", @c "SVE",
 *        @c "EMU128".
 *
 * Decided on first use from what the CPU reports and what the build compiled,
 * and stable for the life of the process.
 *
 * @note Not every target exists on every toolchain. Highway disables AVX-512
 *       (@c HWY_AVX3 and everything above it) under MSVC, so the same source
 *       on the same machine reaches @c "AVX3" from the MinGW, Clang and
 *       AppleClang presets and stops at @c "AVX2" from @c windows-msvc. That
 *       is the library's call, not this project's, and it is the same posture
 *       the sanitizer presets take: a capability that is not really there is
 *       not offered.
 */
[[nodiscard]] CORE_EXPORT std::string_view active_target() noexcept;

/// @brief Bytes in one vector register on the dispatched target - 16 for SSE2
///        and NEON, 32 for AVX2, 64 for AVX-512.
/// @note Divide by the element size for the lane count the kernels step by.
///       On a scalable target (SVE, RVV) this is the width the hardware
///       currently reports rather than a compile-time constant, which is the
///       reason it is a function.
[[nodiscard]] CORE_EXPORT std::size_t register_bytes() noexcept;

} // namespace exchange::core::simd
