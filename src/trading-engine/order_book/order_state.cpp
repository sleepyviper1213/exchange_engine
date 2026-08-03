#include "order_state.hpp"

#include <cassert>

namespace exchange::engine {

order_state::order_state(quantity_t initial_quantity) noexcept
	: quantity_(initial_quantity), remaining_(initial_quantity) {
	// JML: requires initial_quantity > 0. There is no order_state for a
	// non-positive order, so the validation boundary must reject first.
	assert(initial_quantity > 0 && "order quantity must be positive");
}

void order_state::apply_fill(quantity_t lots) noexcept {
	assert(is_active() && "fill on a terminal order");
	assert(lots > 0 && "fill quantity must be positive");
	assert(lots <= remaining_ && "overfill: fill exceeds remaining quantity");
	remaining_ -= lots;
}

void order_state::modify(quantity_t new_quantity) noexcept {
	assert(is_active() && "modify on a terminal order");
	// JML: requires new_quantity > traded. Resizing to at or below the executed
	// quantity is a cancel, not a modify, and the caller must route it there.
	assert(new_quantity > traded() && "modify below executed quantity");
	remaining_ = new_quantity - traded();
	quantity_  = new_quantity;
}

void order_state::cancel() noexcept {
	// The fill/cancel race: an order that filled first is already terminal, and
	// its cancel request is declined by the caller rather than applied here.
	assert(is_active() && "cancel on a terminal order");
	cancelled_ = true;
}

OrderStatus order_state::status() const noexcept {
	// Order matters: a cancel freezes whatever was executed, so the flag wins
	// over the partial-fill reading of the same quantities.
	if (cancelled_) return OrderStatus::CANCELLED;
	if (remaining_ == 0) return OrderStatus::FILLED;
	if (remaining_ == quantity_) return OrderStatus::LIVE;
	return OrderStatus::PARTIALLY_FILLED;
}

} // namespace exchange::engine
