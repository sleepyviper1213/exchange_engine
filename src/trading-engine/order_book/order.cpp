#include "order.hpp"

namespace order_book {

bool Order::is_buy() const noexcept { return side == Side::BID; }

bool Order::has_quantity() const noexcept { return volume > 0; }

void Order::decrease_volume_by(Volume volume) noexcept {
	this->volume -= volume;
}

} // namespace order_book