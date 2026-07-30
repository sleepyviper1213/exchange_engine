#include "order_list.hpp"

namespace exchange::engine::detail {

bool order_list::is_empty() const { return head == NO_NODE; }

void order_list::push_back(order_pool &pool, node_index node, quantity_t volume) {
	if (tail == NO_NODE) {
		head = tail = node;
	} else {
		pool.get(tail).next = node;
		pool.get(node).prev = tail;
		tail                = node;
	}
	total_volume += volume;
	++count;
}

node_index order_list::pop_front(order_pool &pool) {
	const node_index node = head;
	total_volume -= pool.get(node).value.qty();
	--count;
	head = pool.get(node).next;
	if (head != NO_NODE) pool.get(head).prev = NO_NODE;
	else tail = NO_NODE;
	return node;
}

void order_list::unlink(order_pool &pool, node_index node) {
	total_volume -= pool.get(node).value.qty();
	--count;
	const node_index prev = pool.get(node).prev;
	const node_index next = pool.get(node).next;
	if (prev != NO_NODE) pool.get(prev).next = next;
	else head = next;
	if (next != NO_NODE) pool.get(next).prev = prev;
	else tail = prev;
}

resting_order &order_list::front(order_pool &pool) { return pool.get(head).value; }

node_index order_list::back() const noexcept { return tail; }

quantity_t order_list::aggregate_resting_volume() const noexcept { return total_volume; }

std::size_t order_list::size() const noexcept { return count; }

void order_list::reduce_front(order_pool &pool, quantity_t amount) {
	pool.get(head).value.decrease_volume_by(amount);
	total_volume -= amount;
}

void order_list::reset_to_single(order_pool &pool, quantity_t volume) {
	for (node_index n = pool.get(head).next; n != NO_NODE;) {
		const node_index next = pool.get(n).next;
		pool.deallocate(n);
		n = next;
	}
	auto &node   = pool.get(head);
	node.next    = NO_NODE;
	tail         = head;
	node.value   = resting_order(node.value.id(), volume);
	total_volume = volume;
	count        = 1;
}

} // namespace exchange::engine::detail
