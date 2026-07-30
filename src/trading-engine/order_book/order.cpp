#include "order.hpp"

namespace exchange::engine {

bool Order::is_buy() const noexcept { return side == side_t::bid; }

bool Order::has_quantity() const noexcept { return qty > 0; }

void Order::decrease_volume_by(quantity_t v) noexcept {
	this->qty -= v;
}

} // namespace exchange::engine