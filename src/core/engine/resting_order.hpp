#pragma once

#include "types.hpp"

namespace core::engine {

/// @brief A resting order living in the pool; FIFO links come from Node<T>.
class RestingOrder {
public:
	RestingOrder() = default;
	RestingOrder(OrderId id, Volume volume) noexcept;

	[[nodiscard]] OrderId id() const noexcept;
	[[nodiscard]] Volume volume() const noexcept;
	[[nodiscard]] bool has_quantity() const noexcept;
	void decrease_volume_by(Volume amount) noexcept;
private:
	OrderId id_;
	Volume volume_;
};

} // namespace core::engine