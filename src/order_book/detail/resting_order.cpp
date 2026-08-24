#include "resting_order.hpp"

namespace exchange::engine::detail {

resting_order::resting_order(order_id_t id, quantity_t quantity) noexcept
	: id_(id), state_(quantity) {}

resting_order::resting_order(order_id_t id, const order_state &state) noexcept
	: id_(id), state_(state) {}

order_id_t resting_order::id() const noexcept { return id_; }

quantity_t resting_order::qty() const noexcept { return state_.remaining(); }

bool resting_order::has_quantity() const noexcept { return qty() > 0; }

const order_state &resting_order::state() const noexcept { return state_; }

void resting_order::decrease_volume_by(quantity_t amount) noexcept {
	state_.apply_fill(amount);
}

void resting_order::cancel() noexcept { state_.cancel(); }

} // namespace exchange::engine::detail
