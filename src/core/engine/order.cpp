#include "order.hpp"

namespace core::engine {

bool Order::is_buy() const noexcept { return side == Side::BID; }

bool Order::has_quantity() const noexcept { return volume > 0; }

void Order::decrease_volume_by(Volume volume) noexcept {
	this->volume -= volume;
}

} // namespace core::engine