#pragma once

#include "core/types.hpp"

namespace exchange::engine::detail {

/// @brief A resting order living in the pool; FIFO links come from Node<T>.
class resting_order {
public:
	resting_order() = default;
	resting_order(order_id id, quantity v) noexcept;

	[[nodiscard]] order_id id() const noexcept;
	[[nodiscard]] quantity qty() const noexcept;
	[[nodiscard]] bool has_quantity() const noexcept;
	void decrease_volume_by(quantity amount) noexcept;

private:
	order_id id_;
	quantity volume_;
};

} // namespace exchange::engine::detail
