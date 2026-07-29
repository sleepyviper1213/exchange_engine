#include "order.hpp"

namespace exchange::engine {

bool Order::is_buy() const noexcept { return side == side::bid; }

bool Order::has_quantity() const noexcept { return qty > 0; }

void Order::decrease_volume_by(quantity v) noexcept {
	this->qty -= v;
}

} // namespace exchange::engine