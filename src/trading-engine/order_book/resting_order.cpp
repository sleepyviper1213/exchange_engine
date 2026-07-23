#include "resting_order.hpp"

namespace order_book {

RestingOrder::RestingOrder(OrderId id, Volume volume) noexcept
	: id_(id), volume_(volume) {}

OrderId RestingOrder::id() const noexcept { return id_; }

Volume RestingOrder::volume() const noexcept { return volume_; }

bool RestingOrder::has_quantity() const noexcept { return volume_ > 0; }

void RestingOrder::decrease_volume_by(Volume amount) noexcept {
	volume_ -= amount;
}

} // namespace order_book
