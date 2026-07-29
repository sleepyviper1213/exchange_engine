#include "resting_order.hpp"

namespace exchange::engine::detail {

resting_order::resting_order(order_id id, quantity v) noexcept
	: id_(id), volume_(v) {}

order_id resting_order::id() const noexcept { return id_; }

quantity resting_order::qty() const noexcept { return volume_; }

bool resting_order::has_quantity() const noexcept { return volume_ > 0; }

void resting_order::decrease_volume_by(quantity amount) noexcept {
	volume_ -= amount;
}

} // namespace exchange::engine::detail
