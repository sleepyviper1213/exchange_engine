#include "level.hpp"

namespace exchange::engine {

void Level::add_order(detail::order_pool &pool, const Order &order) {
	const detail::node_index node = pool.allocate();
	pool.get(node).value          = detail::resting_order(order.id, order.qty);
	orders.push_back(pool, node, order.qty);
}

bool Level::has_empty_orders() const noexcept { return orders.is_empty(); }

quantity_t Level::total_volume() const noexcept {
	return orders.aggregate_resting_volume();
}

std::size_t Level::order_count() const noexcept { return orders.size(); }

} // namespace exchange::engine
