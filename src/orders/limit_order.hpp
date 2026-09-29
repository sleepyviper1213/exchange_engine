#pragma once
#include "types.hpp"

namespace exchange::engine::orders {

/// @brief A plain resting limit order. Scaffold placeholder for the richer
///        order taxonomy the strategies build on.
struct limit_order {
	order_id_t id;
	side_t side;
	price_t price;
};

} // namespace exchange::engine::orders
