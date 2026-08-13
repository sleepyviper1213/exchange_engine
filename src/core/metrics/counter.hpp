#pragma once
// A cheap, always-on counter for hot-path metrics.
//
// The concurrency contract is the one risk::position_book already documents
// and measures (risk_management/position.hpp): one writer, any number of
// readers. Every metric this module records lives on a single-owner path —
// a partition's own counters are only ever bumped from that partition's one
// consumer thread, the same way position_book's per-symbol entry is only
// ever bumped by the thread that owns that symbol's strategy host. With one
// writer, a read-modify-write does not need to be atomic, only race-free — a
// relaxed load followed by a relaxed store, with no `lock` prefix, unlike
// `fetch_add`. See position_book's class comment for the fuller argument and
// the measured numbers in docs/performance.md (BM_PositionApplyFill).
//
// Readers still see whole 64-bit values with no tearing; they just may see a
// value that is one update stale, which is what a metric snapshot is for.

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <atomic>
#include <cstdint>
#include <new>

namespace exchange::core::metrics {

namespace detail {

/// @brief The single-writer relaxed bump: load, add, store, all relaxed.
/// Shared by counter and histogram so the two don't each spell out the same
/// non-obvious "this is not a race" argument.
void bump_relaxed(std::atomic<std::uint64_t> &slot,
				  std::uint64_t delta) noexcept;

} // namespace detail

/**
 * @brief One named quantity, cache-line isolated so unrelated counters never
 *        false-share.
 *
 * The isolation is for counters that live *beside* other counters a
 * different thread may be bumping concurrently — e.g. two fields on two
 * different partition_metrics structs pinned to two different cores.
 * histogram.hpp deliberately does not reuse this type for its own buckets:
 * all of a histogram's buckets are written by the same single thread, so
 * there is nothing to isolate them from and paying a cache line per bucket
 * would only bloat the structure.
 *
 * @warning Single-writer, like position_book. Constructing, bumping and
 *          resetting from more than one thread at a time is a race this type
 *          does nothing to prevent.
 *
 * The class itself is not marked @c CORE_EXPORT — only its members are (MSVC
 * C2487 on a dll-interface class with a nested/dependent member) — the same
 * shape @c execution::order_manager already uses.
 */
class alignas(std::hardware_destructive_interference_size) counter {
public:
	counter() noexcept = default;

	// Not copyable or movable: a counter's identity is its address, which is
	// what a registry (registry.hpp) records a pointer to.
	counter(const counter &)            = delete;
	counter &operator=(const counter &) = delete;
	counter(counter &&)                 = delete;
	counter &operator=(counter &&)      = delete;
	~counter()                          = default;

	/// @brief Writer side: add @p delta. One thread only — see the class note.
	CORE_EXPORT void add(std::uint64_t delta) noexcept;

	/// @brief Writer side: add one.
	CORE_EXPORT void increment() noexcept;

	/// @brief Reader side: the current value. Any thread.
	[[nodiscard]] CORE_EXPORT std::uint64_t load() const noexcept;

	/// @brief Writer side: back to zero. A session boundary, not a decrement.
	CORE_EXPORT void reset() noexcept;

private:
	std::atomic<std::uint64_t> value_{0};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
			  "a metrics counter that takes a lock would put a mutex on "
			  "whatever hot path records it");

} // namespace exchange::core::metrics
