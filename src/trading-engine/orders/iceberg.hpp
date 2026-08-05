#pragma once
#include "types.hpp"

namespace exchange::engine::orders {

/// @brief Resting iceberg order: only @c visible is exposed to the book, while
///        @c remaining_amount is drawn down from the hidden reserve and the
///        visible slice replenished as it fills.
struct IcebergOrder {
	order_id_t id;
	side_t side;
	price_t price;
	quantity_t total_amount;
	quantity_t visible;
	quantity_t remaining_amount;
};

} // namespace exchange::engine::orders
