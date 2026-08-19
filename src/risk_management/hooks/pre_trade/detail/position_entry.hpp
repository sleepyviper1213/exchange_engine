#pragma once
// How a position is actually stored, which is nobody's business but
// `position_book`'s.
//
// The counters are the only atomics in the module and the padding around them
// is load-bearing, so both are documented here rather than left to be inferred
// from a private member list. What `position_book` publishes is
// `position_snapshot`; this is what it keeps.

#include "trading-engine/orders/types.hpp"

#include <atomic>
#include <cstdint>
#include <new>

namespace exchange::risk::hooks::pre_trade::detail {

/**
 * @brief One listing's counters, alone on a cache line.
 *
 * Two threads updating two symbols never share a line. Six 8-byte counters is
 * 48 bytes, which fits with room to spare; the padding is the point and is not
 * waste.
 *
 * @see position_book for the single-writer contract these are read and written
 *      under, and for why every access is relaxed.
 */
struct alignas(std::hardware_destructive_interference_size) position_entry {
	std::atomic<volume_t> net_lots{0};
	std::atomic<std::int64_t> net_notional{0};
	std::atomic<volume_t> bought_lots{0};
	std::atomic<volume_t> sold_lots{0};
	std::atomic<volume_t> working_bid_lots{0};
	std::atomic<volume_t> working_ask_lots{0};
};

static_assert(std::atomic<volume_t>::is_always_lock_free,
			  "a position counter that takes a lock would put a mutex on the "
			  "fill path");

/**
 * @brief The single-writer read-modify-write.
 *
 * A relaxed load and a relaxed store rather than @c fetch_add, which is sound
 * only because one thread writes a given entry: nobody can interleave with the
 * pair, so it does not have to be atomic, only race-free. It compiles to
 * `mov / add / mov` with no @c lock prefix. @see position_book.
 */
void bump(std::atomic<volume_t> &counter, volume_t delta) noexcept;

/// @brief The working-quantity counter for @p side.
[[nodiscard]] std::atomic<volume_t> &working(position_entry &e,
											 side_t side) noexcept;

/// @brief @c working for a reader.
[[nodiscard]] const std::atomic<volume_t> &working(const position_entry &e,
												   side_t side) noexcept;

} // namespace exchange::risk::hooks::pre_trade::detail
