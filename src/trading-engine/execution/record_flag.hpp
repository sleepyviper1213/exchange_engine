#pragma once
// The venue-level facts an order record carries as bits.
//
// Split from order_manager.hpp because the flags are part of what a *record*
// is, and a reader of one — a journal, a client report, the outcome builder —
// has no reason to also compile the slot table that stores it.

#include "core/util/flag.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine::execution {

/**
 * @brief Venue-level facts about an order that its quantities cannot express.
 *
 * A flag set rather than a @c bool per fact, because these arrive one at a time
 * as the venue grows: self-trade prevention, post-only rejection and short-sell
 * marking are all bits an @c order_record will want, and each added as its own
 * @c bool would cost a byte and a new accessor. Here they cost a bit and
 * nothing else — @c order_record is 48 bytes with one flag or with eight.
 *
 * Deliberately not folded into @c order_state's own packed cancellation bit:
 * that word is 8 bytes on every pool node and each bit of it comes out of the
 * quantity's range, so a venue-level fact would be paying the matching loop's
 * budget for something the matching loop never reads. This byte is the
 * manager's, above the book, where there is room.
 */
enum class record_flag : std::uint8_t {
	/// @brief The order never entered the book. The one status the quantities
	///        cannot express — nothing traded with the remainder withdrawn is
	///        exactly what a cancel that never filled looks like.
	REJECTED = 1U << 0U,
};

EXCHANGE_ENABLE_FLAGS(record_flag)

/// @brief The venue-level facts recorded about one order.
using record_flags = core::util::flag<record_flag>;

} // namespace exchange::engine::execution
