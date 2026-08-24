#include "order.hpp"

namespace exchange::engine::orders {

bool order::is_buy() const noexcept { return side == side_t::bid; }

bool order::has_quantity() const noexcept { return qty > 0; }

void order::decrease_volume_by(quantity_t v) noexcept {
	this->qty -= v;
}

} // namespace exchange::engine::orders