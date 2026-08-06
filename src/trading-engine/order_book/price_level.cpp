#include "price_level.hpp"

#include <cassert>

namespace exchange::engine {

detail::resting_order *price_level::add_order(detail::order_pool &pool,
									   const orders::order &order) {
	detail::resting_order *node = pool.acquire(order.id, order.qty);
	if (node == nullptr) [[unlikely]] return nullptr;
	orders.push_back(*node);
	volume += order.qty;
	return node;
}

detail::resting_order *price_level::add_order(detail::order_pool &pool, order_id_t id,
									   const order_state &state) {
	detail::resting_order *node = pool.acquire(id, state);
	if (node == nullptr) [[unlikely]] return nullptr;
	orders.push_back(*node);
	// The aggregate tracks unexecuted quantity, so it takes the remainder — not
	// the order's original size, part of which has already traded.
	volume += state.remaining();
	return node;
}

bool price_level::has_empty_orders() const noexcept { return orders.empty(); }

volume_t price_level::total_volume() const noexcept { return volume; }

std::size_t price_level::order_count() const noexcept { return orders.size(); }

detail::resting_order &price_level::front() noexcept {
	assert(!orders.empty() && "front() on an empty level");
	return orders.front();
}

void price_level::fill_front(quantity_t amount) noexcept {
	front().decrease_volume_by(amount);
	volume -= amount;
}

void price_level::pop_front(detail::order_pool &pool) noexcept {
	assert(!orders.empty() && "pop_front() on an empty level");
	detail::resting_order &head = orders.front();
	volume -= head.qty();
	orders.pop_front();
	pool.release(&head);
}

void price_level::unlink(detail::order_pool &pool,
				   detail::resting_order &node) noexcept {
	volume -= node.qty();
	// s_iterator_to, not a search: the hook lives at a fixed offset inside the
	// node, so the list can be re-entered from the node itself.
	orders.erase(detail::order_list::s_iterator_to(node));
	pool.release(&node);
}

void price_level::release_orders(detail::order_pool &pool) noexcept {
	orders.clear_and_dispose([&pool](detail::resting_order *node) noexcept {
		pool.release(node);
	});
	volume = 0;
}

} // namespace exchange::engine
