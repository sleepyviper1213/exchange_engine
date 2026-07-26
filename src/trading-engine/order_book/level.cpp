#include "level.hpp"

#include <numeric>

namespace exchange::engine {

void Level::add_order(const Order &order) {
	orders.push_back(order);
}

bool Level::has_empty_orders() const noexcept { return orders.empty(); }

Volume Level::total_volume() const noexcept {
	return std::accumulate(orders.begin(), orders.end(), Volume{0},
						   [](Volume sum, const Order &o) {
							   return sum + o.volume;
						   });
}

} // namespace exchange::engine