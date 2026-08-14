#include "order_state.hpp"

#include <cassert>
#include <limits>

namespace exchange::engine {

namespace {

/// @brief The largest quantity the packed 31-bit quantity field can hold.
///
/// Equal to @c quantity_t's own maximum, because that type is a signed 32 and
/// the field gives up only its sign bit — an order's quantity is positive by
/// invariant, so nothing representable is lost. Stated once here so the
/// assertions below name the bound rather than restating the arithmetic.
constexpr quantity_t MAX_QUANTITY = std::numeric_limits<quantity_t>::max();

} // namespace

order_state::order_state(quantity_t initial_quantity) noexcept
	: quantity_and_flag_(static_cast<std::uint32_t>(initial_quantity)),
	  remaining_(initial_quantity) {
	// There is no order_state for a non-positive order, so the validation
	// boundary must reject one before it ever gets here.
	assert(initial_quantity > 0 && "order quantity must be positive");
	// And none for one past the field, which symbol_spec::quantity_from_scaled
	// refuses with QUANTITY_OUT_OF_RANGE before an order is ever built. The
	// bound is what makes the raw store above safe: a value in range never
	// reaches bit 31, so it cannot be mistaken for a cancellation.
	assert(initial_quantity <= MAX_QUANTITY && "order quantity out of range");
}

void order_state::apply_fill(quantity_t lots) noexcept {
	assert(is_active() && "fill on a terminal order");
	assert(lots > 0 && "fill quantity must be positive");
	assert(lots <= remaining_ && "overfill: fill exceeds remaining quantity");
	remaining_ -= lots;
}

void order_state::modify(quantity_t new_quantity) noexcept {
	assert(is_active() && "modify on a terminal order");
	// Resizing to at or below the executed quantity is a cancel, not a modify,
	// and the caller must route it there.
	assert(new_quantity > traded() && "modify below executed quantity");
	assert(new_quantity <= MAX_QUANTITY && "modified quantity out of range");
	remaining_ = new_quantity - traded();
	// Rewrite the quantity while preserving the flag. modify() is only reachable
	// on an active order, so the bit is clear and the OR is a formality — but
	// writing it this way means the pack has exactly one assignment idiom, and
	// no future caller has to remember which half it is allowed to clobber.
	quantity_and_flag_ = (quantity_and_flag_ & CANCELLED_BIT) |
						 static_cast<std::uint32_t>(new_quantity);
}

void order_state::cancel() noexcept {
	// The fill/cancel race: an order that filled first is already terminal, and
	// its cancel request is declined by the caller rather than applied here.
	assert(is_active() && "cancel on a terminal order");
	quantity_and_flag_ |= CANCELLED_BIT;
}

[[nodiscard]] quantity_t order_state::quantity() const noexcept {
	return static_cast<quantity_t>(quantity_and_flag_ & QUANTITY_MASK);
}

[[nodiscard]] quantity_t order_state::traded() const noexcept {
	return quantity() - remaining_;
}

[[nodiscard]] quantity_t order_state::remaining() const noexcept {
	return remaining_;
}


OrderStatus order_state::status() const noexcept {
	// Order matters: a cancel freezes whatever was executed, so the flag wins
	// over the partial-fill reading of the same quantities.
	if ((quantity_and_flag_ & CANCELLED_BIT) != 0) return OrderStatus::CANCELLED;
	if (remaining_ == 0) return OrderStatus::FILLED;
	if (remaining_ == quantity()) return OrderStatus::LIVE;
	return OrderStatus::PARTIALLY_FILLED;
}

[[nodiscard]] bool order_state::is_active() const noexcept {
	return engine::is_active(status());
}


} // namespace exchange::engine
