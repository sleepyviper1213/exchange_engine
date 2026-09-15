#pragma once
// Cache geometry, as the host reports it at run time.
//
// The compile-time side of the same subject is cache.hpp next door, and
// these share its namespace on purpose: CACHE_LINE_SIZE and cache_line_size
// answer one question, one by asking the compiler and one by asking the
// machine, and the whole point of having both is that they can disagree.
// Kept out of cache.hpp because probing drags in <windows.h> or <unistd.h>,
// and cache.hpp is included by half the tree.

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <cstddef>

namespace exchange::core::concurrency {

/**
 * @brief Bytes in the last-level data cache, or a conservative guess.
 *
 * "Last level" rather than "L3" on purpose: Apple silicon has no L3 in the
 * x86 sense and plenty of server parts make L2 the last shared level, so a
 * query pinned to level 3 reports zero on exactly the hosts it matters on.
 * This reports the deepest data or unified cache the OS admits to, which is
 * the same definition @c topology::share_llc groups cores by.
 *
 * @return The size in bytes, or 8 MiB where the host will not say. The
 *         fallback is a guess and is meant to be one - it keeps a caller that
 *         clamps to this value from clamping to zero and prefetching nothing.
 *
 * @note Probes the OS on each call rather than caching: the callers are
 *       start-up and sizing decisions, not the matching path. Do not put this
 *       in a loop.
 */
[[nodiscard]] CORE_EXPORT std::size_t last_level_cache_size() noexcept;

/**
 * @brief Bytes in one cache line, as the OS reports it.
 *
 * The runtime counterpart to @c CACHE_LINE_SIZE, and the reason it exists: that
 * constant is whatever the *compiler* was told, which is a different thing from
 * what the machine has. GCC and clang both answer 64 on arm64 while Apple
 * silicon runs a 128-byte line, so a build for that target pads to half a line
 * and gets no diagnostic from the toolchain. Comparing the two is what turns
 * that from a documented hazard into a caught one.
 *
 * @return The line size in bytes, or 64 where the host will not say. Reports
 *         the L1 data cache's line, which is the granularity coherence and
 *         false sharing operate at.
 *
 * @note Probes the OS on each call. @see last_level_cache_size
 */
[[nodiscard]] CORE_EXPORT std::size_t cache_line_size() noexcept;

} // namespace exchange::core::concurrency
