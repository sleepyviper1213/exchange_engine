#pragma once
// Pulling a buffer into cache before the code that needs it runs.
//
// Two ways to do that, and they are not interchangeable:
//
//   warm_by_hint   issues a prefetch per line. Costs almost nothing, promises
//                  nothing - the hardware may drop every one of them, and a
//                  hint that arrives too late did nothing but retire.
//   warm_by_touch  reads a byte per line. Guarantees the line is resident
//                  because the load has to happen, and pays every miss up
//                  front, on this thread, now.
//
// Reach for the hint on a path that is about to walk the data anyway and can
// absorb a miss if the hint misses; reach for the touch when the *next* access
// must not miss and this moment is the cheap place to pay - warming a journal
// buffer at start-up, faulting in an arena before a session opens.
//
// Both clamp to what the cache can hold. Warming more than that is not a
// stronger version of warming: the tail evicts the head, and a caller that
// asked for 64 MiB gets the last few MiB resident and the rest of its working
// set thrown away. The clamp is not optional here for that reason - the
// unclamped primitive is prefetch_range next door, which says in its own
// warning that the budget is the caller\'s problem.

#include "core/concurrency/cache.hpp"       // CACHE_LINE_SIZE
#include "core/concurrency/cache_probe.hpp" // last_level_cache_size
#include "prefetch.hpp"                     // prefetch_range, locality_hint

#include <algorithm>
#include <cstddef>

namespace exchange::core::optimisation {

/**
 * @brief @p bytes, clamped to what the last-level cache can actually hold.
 *
 * @note Queries the OS on every call. @see concurrency::last_level_cache_size
 */
[[nodiscard]] inline std::size_t warmable_bytes(std::size_t bytes) noexcept {
	return std::min(bytes, concurrency::last_level_cache_size());
}

/**
 * @brief Ask for @p bytes from @p data to be pulled in, one hint per line.
 *
 * @param data Start of the region. Safe to be null or unmapped - a prefetch
 *        never faults, so this may run before the buffer is populated.
 * @param bytes How much to ask for, clamped by @c warmable_bytes.
 * @param hint How soon it is wanted. The default says the caller will use the
 *        data repeatedly; pass @c locality_hint::none for a pass that reads
 *        once and should not disturb the working set.
 *
 * @note Advisory. Nothing here guarantees a single line is resident when this
 *       returns, which is the difference from @c warm_by_touch.
 */
inline void warm_by_hint(const void *data, std::size_t bytes,
						 locality_hint hint = locality_hint::high) noexcept {
	prefetch_range(data, warmable_bytes(bytes), hint);
}

/**
 * @brief Read one byte per line from @p data, forcing the lines resident.
 *
 * @param data Start of the region. Unlike @c warm_by_hint this **dereferences**
 *        - @p data must point at @p bytes of live, readable storage, and
 *        overrunning it is undefined rather than merely wasteful.
 * @param bytes How much to touch, clamped by @c warmable_bytes.
 *
 * @note The read goes through a @c volatile sink. Without it the loop reads
 *       memory, does nothing with the result, and the optimiser deletes the
 *       whole thing - which is a silent no-op rather than a compile error, and
 *       exactly the failure this function exists to avoid.
 *
 * @warning Costs every miss it resolves, on the calling thread, before it
 *          returns. That is the point, and it is why this does not belong on a
 *          path that is being measured for latency.
 */
inline void warm_by_touch(const void *data, std::size_t bytes) noexcept {
	const auto *const first = static_cast<const std::byte *>(data);
	const std::size_t len   = warmable_bytes(bytes);
	for (std::size_t offset = 0; offset < len;
		 offset += concurrency::CACHE_LINE_SIZE) {
		const volatile std::byte sink = first[offset];
		(void)sink;
	}
}

} // namespace exchange::core::optimisation
