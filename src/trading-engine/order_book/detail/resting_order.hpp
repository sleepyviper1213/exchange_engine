#pragma once

#include "core/types.hpp"

namespace exchange::engine::detail {

/// @brief A resting order living in the pool; FIFO links come from Node<T>.
class resting_order {
public:
	resting_order() = default;
	resting_order(order_id_t id, quantity_t v) noexcept;

	[[nodiscard]] order_id_t id() const noexcept;
	[[nodiscard]] quantity_t qty() const noexcept;
	[[nodiscard]] bool has_quantity() const noexcept;
	void decrease_volume_by(quantity_t amount) noexcept;

private:
	order_id_t id_;
	quantity_t volume_;
};

} // namespace exchange::engine::detail
