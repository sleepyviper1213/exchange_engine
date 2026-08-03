#pragma once

#include "../order_state.hpp"
#include "core/types.hpp"

namespace exchange::engine::detail {

/// @brief A resting order living in the pool; FIFO links come from Node<T>.
///
/// Carries the full @c order_state rather than a bare remaining quantity, so
/// the cumulative executed quantity an outcome report needs is on the node
/// itself and every fill goes through the verified state machine. That costs 8
/// bytes per node over the old {id, qty} pair; it is what makes an order's fate
/// reportable at all.
///
/// A resting order is always active — LIVE or PARTIALLY_FILLED. The book has no
/// representation for a terminal one: filling it to zero or cancelling it
/// unlinks the node in the same step, so the TLA+ property
/// `TerminalStatusNeverChanges` holds structurally rather than by check.
class resting_order {
public:
	resting_order() = default;
	resting_order(order_id_t id, quantity_t v) noexcept;

	/// @brief Rest an order that already exists, keeping the lifecycle it has.
	///
	/// The path an aggressor takes when part of it crossed and the remainder
	/// rests: the node continues the *same* @c order_state, so its later fills
	/// report cumulative quantities against the original order rather than
	/// against the leftover. Resting a partial remainder as a fresh order is
	/// what would make a 10-lot order that filled 4 then 6 report two separate
	/// completions.
	resting_order(order_id_t id, const order_state &state) noexcept;

	[[nodiscard]] order_id_t id() const noexcept;

	/// @brief Unexecuted quantity still resting. @see order_state::remaining
	[[nodiscard]] quantity_t qty() const noexcept;

	[[nodiscard]] bool has_quantity() const noexcept;

	/// @brief The order's lifecycle state, for reporting an outcome.
	[[nodiscard]] const order_state &state() const noexcept;

	/// @brief Execute @p amount against this order.
	///
	/// Named for what it does to the book (the level's aggregate shrinks) while
	/// delegating to @c order_state::apply_fill, which is where the overfill
	/// and terminal-order preconditions are checked.
	void decrease_volume_by(quantity_t amount) noexcept;

	/// @brief Mark the order withdrawn, freezing its executed quantity.
	///        The caller unlinks the node immediately afterwards.
	void cancel() noexcept;

private:
	order_id_t id_;
	/// Default-constructed nodes exist only as unallocated pool slots, never as
	/// orders; the quantity here just satisfies order_state's positive-quantity
	/// invariant until a real order overwrites the slot.
	order_state state_{1};
};

} // namespace exchange::engine::detail
