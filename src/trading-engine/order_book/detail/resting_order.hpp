#pragma once

#include "../order_state.hpp"
#include "trading-engine/orders/types.hpp"

#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/list_hook.hpp>

namespace exchange::engine::detail {

/**
 * @brief The FIFO link a resting order carries into its price level.
 *
 * @c safe_link rather than @c normal_link: the extra work is zeroing the node's
 * pointers on unlink, and what it buys is that a double-link or a still-linked
 * destruction trips an assertion instead of quietly corrupting a level's FIFO.
 * On this path a corrupted list is a wrong fill, so the trade is worth it.
 *
 * Not @c auto_unlink, which would rule out @c constant_time_size and make a
 * level's order count an O(n) walk. Unlinking goes through the list with
 * @c s_iterator_to, which is O(1) all the same — the hook is inside the node,
 * so nothing has to be searched for.
 */
using fifo_hook = boost::intrusive::list_member_hook<
	boost::intrusive::link_mode<boost::intrusive::safe_link>>;

/**
 * @brief One order resting in the book: a pool cell that is also a list node.
 *
 * The structural metadata is embedded rather than wrapped. A node *is* the
 * order, so resting one costs a single pool cell and no separate link
 * allocation, and the matching loop reads an order's id and quantity out of the
 * same cache line it followed the @c next pointer through.
 *
 * Carries the whole @c order_state, not a bare remaining quantity, so the
 * cumulative executed quantity an outcome report needs is on the node itself
 * and every fill goes through the state machine. That costs 8 bytes a node; it
 * is what makes an order's fate reportable at all.
 *
 * A resting order is always active — LIVE or PARTIALLY_FILLED. The book has no
 * representation for a terminal one: filling it to zero or cancelling it
 * unlinks the node in the same step, so "a terminal order never changes again"
 * holds structurally rather than by check.
 */
class resting_order {
public:
	/**
	 * @brief The level's FIFO link.
	 *
	 * Public because @c boost::intrusive::member_hook needs a pointer to it,
	 * and structural rather than business state: it belongs to whichever
	 * @c order_list currently holds this node, and nothing else may touch it.
	 * Placed first so the id and quantity a match reads sit on the same line
	 * the list walk already pulled in.
	 */
	fifo_hook hook;

	/// @brief A brand-new order of @p quantity lots, nothing executed.
	resting_order(order_id_t id, quantity_t quantity) noexcept;

	/**
	 * @brief Rest an order that already exists, keeping the lifecycle it has.
	 *
	 * The path an aggressor takes when part of it crossed and the remainder
	 * rests: the node continues the *same* @c order_state, so its later fills
	 * report cumulative quantities against the original order. Resting the
	 * remainder as a fresh order is what would make a 10-lot order that filled
	 * 4 and then 6 report two separate completions.
	 */
	resting_order(order_id_t id, const order_state &state) noexcept;

	[[nodiscard]] order_id_t id() const noexcept;

	/// @brief Unexecuted quantity still resting. @see order_state::remaining
	[[nodiscard]] quantity_t qty() const noexcept;

	[[nodiscard]] bool has_quantity() const noexcept;

	/// @brief The order's lifecycle state, for reporting an outcome.
	[[nodiscard]] const order_state &state() const noexcept;

	/// @brief Execute @p amount against this order.
	///
	/// Named for what it does to the book — the level's aggregate shrinks by
	/// the same amount — while delegating to @c order_state::apply_fill, which
	/// is where the overfill and terminal-order preconditions live.
	void decrease_volume_by(quantity_t amount) noexcept;

	/// @brief Mark the order withdrawn, freezing its executed quantity. The
	///        caller unlinks the node immediately afterwards.
	void cancel() noexcept;

private:
	order_id_t id_;
	order_state state_;
};

// A node is what the matching loop walks; two of them per cache line is the
// point of embedding the hook rather than wrapping the order in one.
//
// Exactly 32, not merely "within 64". The loose bound was satisfied at 48 bytes
// — which is 1.33 nodes per line, so nodes straddled line boundaries and the
// claim above was not true of the code asserting it. 16 (hook) + 8 (id) +
// 8 (state) is what actually delivers two per line, and an equality assert is
// what keeps it delivered: any field added here has to come out of the budget
// or move the number deliberately.
static_assert(sizeof(resting_order) == 32,
			  "a resting order must stay half a cache line — see order_state");

} // namespace exchange::engine::detail
