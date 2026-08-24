#pragma once
// Helpers shared by the order_manager suites. Every one of them admits orders
// and then asks what became of them, and the only thing they disagree about is
// which lifecycle they drive - so the order builder lives here rather than being
// copied five times.
//
// The manager is spelled `manager` in every suite, never `orders`: that name is
// the `exchange::engine::orders` namespace, and a local shadowing it makes
// `orders::order_type` stop compiling halfway down a file.

#include "orders/order.hpp"
#include "orders/types.hpp"

namespace order_manager_test {

using exchange::order_id_t;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::orders::order;

/// @brief A plain GTC limit buy - the shape a suite reaches for when the only
///        field it cares about is the id.
[[nodiscard]] inline order limit(order_id_t id, quantity_t qty = 10,
								 price_t price = 100) {
	return order{.id = id, .side = side_t::bid, .price = price, .qty = qty};
}

} // namespace order_manager_test
