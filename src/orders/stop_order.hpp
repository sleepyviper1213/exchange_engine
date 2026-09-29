#pragma once
#include "types.hpp"

namespace exchange::engine::orders {

/// @brief A stop order that becomes marketable once the tape trades through
///        @c trigger_price. Scaffold: not yet wired into the matching path.
struct stop_order {
	order_id_t id;
	side_t side;
	price_t trigger_price;
	quantity_t volume;
};

} // namespace exchange::engine::orders
