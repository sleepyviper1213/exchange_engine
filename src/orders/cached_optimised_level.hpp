#pragma once

#include "orders/types.hpp"

#include <cstdint>

namespace exchange::engine::experimental {

/**
 * @brief One aggregate price level: the venue's size at a price, plus running
 *        statistics about how that size has been updated.
 *
 * @par Why this is not cache-line aligned
 * It was, with the stated reason being "padded to avoid false sharing between
 * the hot read fields and the running statistics". That reason does not hold in
 * either direction. False sharing is a phenomenon between *threads*, and this
 * book is single-writer by construction; and `alignas` on the struct pads the
 * struct's tail, which cannot separate two of its own fields from each other
 * anyway. What the annotation actually did was inflate a 32-byte level to 64.
 *
 * The cost of that landed on the operation this class exists to make fast. A
 * side is a sorted @c std::array, so an insert or erase near the touch shifts
 * the tail - at depth 1024 the average shift moves ~512 levels, and every byte
 * of padding is a byte memmoved on the hot path. Halving the level halves the
 * bytes moved, and doubles the levels a cache line holds for the binary search
 * that precedes the shift.
 *
 * @note 32 bytes, asserted below: price and volume in the first 8, statistics in
 *       the rest. Two levels per cache line, four once @c total_volume and
 *       @c avg_order_size move to a parallel cold array - which is the next step
 *       and the reason the hot pair is kept first.
 */
struct cache_optimised_level {
	// --- hot: what the search and the touch read -----------------------------
	price_t price;
	quantity_t volume;

	// --- cold: statistics about the update stream, never read while matching --
	std::uint32_t count;
	std::uint32_t timestamp; ///< low 32 bits of the book's update counter
	std::uint64_t total_volume;
	std::uint32_t avg_order_size;
};

// The number this type exists to keep. A side is an array of these and the
// insert/erase path memmoves the tail, so anything added here is paid on every
// update at every depth below it - an equality assert rather than an upper
// bound, so growth has to be a decision rather than an accident.
static_assert(sizeof(cache_optimised_level) == 32,
			  "a level must stay half a cache line - see the class note");

} // namespace exchange::engine::experimental
