#pragma once

#include "../fwd.hpp"

namespace exchange::engine::detail {

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

} // namespace exchange::engine::detail
