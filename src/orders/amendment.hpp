#pragma once

#include "fwd.hpp"
#include "types.hpp"

#include <type_traits>

namespace exchange::engine::orders {

/**
 * @brief A client's request to change an order it already has resting: the new
 *        price, the new quantity, and which order they are for.
 *
 * @par Why this is a type of its own and not an @c order
 * Because the fields an @c order carries that are missing here are missing on
 * purpose, and an amendment that carried them would be offering to change
 * things a venue does not let you change. Side is fixed - amending a buy into a
 * sell is a different order, and the book would have to move the node between
 * two ladders to honour it. Type and time-in-force are fixed for a stronger
 * reason: a resting order carries neither (@c detail::resting_order is an id
 * and an @c order_state and no room for more), so there is nothing there to
 * amend. Every resting order is GOOD_TILL_CANCELLED as far as matching is
 * concerned, and an amendment cannot make one otherwise.
 *
 * What is left is exactly the two quantities a venue does let a client move,
 * and the identity of the order to move them on.
 *
 * @par The quantity is the order's, not the remainder's
 * @c quantity is the new *order* quantity, counted from the order's inception
 * and therefore including whatever has already executed - the same reading
 * @c order_state::quantity has. So amending a 10-lot order that has filled 4
 * down to 6 leaves 2 resting, and amending it to 4 or less is a request to
 * withdraw what is left, which @c order_book::modify_order routes to a cancel
 * rather than refusing. Expressing it as the *remaining* quantity instead would
 * make the meaning of a request depend on how much filled between the client
 * sending it and the venue applying it, which is exactly the race the absolute
 * reading removes.
 */
struct amendment {
	/// @brief The resting order to amend. Zero - the anonymous sentinel - names
	///        nothing the book indexes, so such a request is dropped in the
	///        silence @c add_order and @c delete_order already keep.
	order_id_t id;

	/// @brief The new limit price, in ticks. Equal to the current one for a
	///        quantity-only amendment; anything else moves the order to a
	///        different level, losing its place in the queue and re-crossing
	///        the book on the way in.
	price_t price;

	/// @brief The new order quantity, in lots. @see the class note on why this
	///        is absolute rather than a delta or a remainder.
	quantity_t quantity;

	/**
	 * @brief Venue receipt time, nanoseconds since the Unix epoch.
	 *
	 * Carried for the same reason @c order::timestamp is, and read in the one
	 * case that order's is: an amendment that changes price re-crosses the
	 * book, and any execution it prints is stamped with this. Zero means "not
	 * stamped". @see engine::trade::timestamp
	 */
	timestamp_t timestamp = 0;

	bool operator==(const amendment &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<amendment>,
			  "an amendment is a union arm of event::command, which travels "
			  "the queue's memcpy batch path");
static_assert(sizeof(amendment) == 24,
			  "an amendment must stay inside event::command's widest arm - a "
			  "PLACE's 40-byte order - or the journal's record stride moves");

} // namespace exchange::engine::orders
