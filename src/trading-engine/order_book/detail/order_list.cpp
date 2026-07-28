#include "order_list.hpp"

namespace exchange::engine::detail {

bool OrderList::is_empty() const { return head == kNull; }

void OrderList::push_back(OrderPool &pool, NodeIndex node, Volume volume) {
	if (tail == kNull) {
		head = tail = node;
	} else {
		pool.get(tail).next = node;
		pool.get(node).prev = tail;
		tail                = node;
	}
	total_volume += volume;
}

NodeIndex OrderList::pop_front(OrderPool &pool) {
	const NodeIndex node = head;
	total_volume -= pool.get(node).value.volume();
	head = pool.get(node).next;
	if (head != kNull) pool.get(head).prev = kNull;
	else tail = kNull;
	return node;
}

void OrderList::unlink(OrderPool &pool, NodeIndex node) {
	total_volume -= pool.get(node).value.volume();
	const NodeIndex prev = pool.get(node).prev;
	const NodeIndex next = pool.get(node).next;
	if (prev != kNull) pool.get(prev).next = next;
	else head = next;
	if (next != kNull) pool.get(next).prev = prev;
	else tail = prev;
}

RestingOrder &OrderList::front(OrderPool &pool) { return pool.get(head).value; }

Volume OrderList::volume() const noexcept { return total_volume; }

void OrderList::reduce_front(OrderPool &pool, Volume amount) {
	pool.get(head).value.decrease_volume_by(amount);
	total_volume -= amount;
}

void OrderList::reset_to_single(OrderPool &pool, Volume volume) {
	for (NodeIndex n = pool.get(head).next; n != kNull;) {
		const NodeIndex next = pool.get(n).next;
		pool.deallocate(n);
		n = next;
	}
	auto &node   = pool.get(head);
	node.next    = kNull;
	tail         = head;
	node.value   = RestingOrder(node.value.id(), volume);
	total_volume = volume;
}

} // namespace exchange::engine::detail
