#pragma once
// What `iceberg` remembers about one parent order while it is being worked.
//
// A private nested struct until now, which in a class template means the public
// header - a template has no private section a reader cannot see. `detail` is
// what says this is bookkeeping rather than interface.

#include "../fwd.hpp"
#include "trading-engine/orders/types.hpp"

namespace exchange::strategy::detail {

/// @brief One parent order, the slice currently showing, and what is left.
struct working_parent {
	order_id_t parent;
	order_id_t child;
	price_t price;
	quantity_t peak;
	quantity_t showing;
	quantity_t reserve;
	side_t side;
	bool active;
};

} // namespace exchange::strategy::detail
