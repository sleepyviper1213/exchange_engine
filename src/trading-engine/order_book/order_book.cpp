#include "order_book.hpp"

#include "core/types.hpp"
#include "detail/book_side.hpp"
#include "level.hpp"
#include "order.hpp"
#include "order_state.hpp"
#include "outcome.hpp"
#include "trade.hpp"

#include <algorithm>
#include <cassert>

namespace exchange::engine {

using detail::book_side;

order_book::order_book(std::size_t capacity)
	: pool_(capacity), bid_(side_t::bid, pool_), ask_(side_t::ask, pool_) {
	index_.reserve(capacity);
}

bool order_book::reject_if_invalid(const order &incoming,
								   std::vector<OrderOutcome> &outcomes) const {
	// order_state has no representation for a non-positive order, so this is
	// the boundary that keeps the invariant true rather than merely asserted.
	if (incoming.qty <= 0) {
		if (incoming.id != kAnonymous)
			outcomes.push_back(
				OrderOutcome::rejected(incoming.id,
									   reject_reason::NON_POSITIVE_QUANTITY,
									   incoming.qty));
		return true;
	}

	// Nothing here watches a trigger price, and a stop order that goes live the
	// instant it arrives is not a stop order. Refusing is the only answer that
	// does not quietly turn one instruction into a different one.
	if (incoming.type == order_type::STOP) {
		if (incoming.id != kAnonymous)
			outcomes.push_back(
				OrderOutcome::rejected(incoming.id,
									   reject_reason::UNSUPPORTED_ORDER_TYPE,
									   incoming.qty));
		return true;
	}

	// Admitting a duplicate id would overwrite index_[id], orphaning the first
	// order's node: it keeps resting and filling, but no cancel can ever reach
	// it. Refusing the second order is the only outcome that leaves every
	// resting order reachable.
	if (incoming.id != kAnonymous && index_.contains(incoming.id)) {
		outcomes.push_back(
			OrderOutcome::rejected(incoming.id,
								   reject_reason::DUPLICATE_ORDER_ID,
								   incoming.qty));
		return true;
	}
	return false;
}

void order_book::place_order(const order &incoming, std::vector<Trade> &trades,
							 std::vector<OrderOutcome> &outcomes) {
	if (reject_if_invalid(incoming, outcomes)) return;

	book_side &opposite    = side_levels(opposed(incoming.side));
	const bool is_reported = incoming.id != kAnonymous;

	// Fill-or-kill is all-or-nothing: if the resting liquidity cannot fully
	// fill the order right now, execute nothing and leave the book untouched.
	if (incoming.tif == time_in_force_instruction::FILL_OR_KILL &&
		!can_fully_fill(opposite,
						incoming.side,
						incoming.price,
						incoming.qty)) {
		if (is_reported)
			outcomes.push_back(
				OrderOutcome::rejected(incoming.id,
									   reject_reason::INSUFFICIENT_LIQUIDITY,
									   incoming.qty));
		return;
	}

	if (is_reported)
		outcomes.push_back(OrderOutcome::accepted(incoming.id, incoming.qty));

	// The aggressor's lifecycle. It outlives the matching loop: if a remainder
	// rests, this same state moves onto the pool node, so the order's traded
	// total keeps accumulating across the crossing and everything after it.
	order_state aggressor{incoming.qty};

	while (aggressor.remaining() > 0 && !opposite.empty()) {
		Level &best = opposite.best();
		if (!is_price_crossing(incoming.side, incoming.price, best.price))
			break;

		while (aggressor.remaining() > 0 && !best.has_empty_orders()) {
			detail::resting_order &resting = best.orders.front(pool_);
			const order_id_t resting_id    = resting.id();
			const quantity_t traded =
				std::min(aggressor.remaining(), resting.qty());

			trades.emplace_back(incoming.id, resting_id, best.price, traded);
			aggressor.apply_fill(traded);
			// Goes through the list so the level's cached aggregate tracks the
			// fill; the reference stays valid, it is the same node.
			best.orders.reduce_front(pool_, traded);

			// Read the passive side's state before pop_front returns its node
			// to the pool — after that the reference is dangling.
			if (resting_id != kAnonymous)
				outcomes.push_back(
					OrderOutcome::fill(resting_id, resting.state()));
			if (is_reported)
				outcomes.push_back(OrderOutcome::fill(incoming.id, aggressor));

			if (!resting.has_quantity()) pop_front(best);
		}
		opposite.remove_best_level_if_empty();
	}

	if (aggressor.remaining() == 0) return;

	// Only GTC rests a remainder; IOC (and a partially-filled FOK, which cannot
	// happen given the pre-check) drop whatever did not cross.
	if (incoming.tif == time_in_force_instruction::GOOD_TILL_CANCELLED) {
		book_side &own     = side_levels(incoming.side);
		const Level &level = own.insert(incoming.id, incoming.price, aggressor);
		if (is_reported)
			index_[incoming.id] =
				Location{incoming.side, incoming.price, level.orders.back()};
		return;
	}

	if (is_reported) {
		// A dropped remainder is a cancellation with a cause, not a rejection:
		// the order was accepted and may well have executed first.
		order_state dropped = aggressor;
		dropped.cancel();
		outcomes.push_back(
			OrderOutcome::cancelled(incoming.id,
									dropped,
									reject_reason::TIME_IN_FORCE));
	}
}

void order_book::place_order(const order &incoming,
							 std::vector<Trade> &trades) {
	std::vector<OrderOutcome> discarded;
	place_order(incoming, trades, discarded);
}

std::vector<Trade> order_book::place_order(const order &incoming) {
	std::vector<Trade> trades;
	place_order(incoming, trades);
	return trades;
}

void order_book::add_order(side_t side, price_t price, quantity_t volume) {
	// Anonymous resting liquidity: no id (untracked for cancel), no matching.
	side_levels(side).insert(
		order{.id = kAnonymous, .side = side, .price = price, .qty = volume});
}

void order_book::cancel_order(order_id_t id,
							  std::vector<OrderOutcome> &outcomes) {
	const auto found = index_.find(id);
	if (found == index_.end()) {
		// The fill/cancel race, resolved in the fill's favour: the order filled
		// and left before this request landed — or was already cancelled, or
		// never existed. One empty index lookup for all three, so the report
		// says only that the cancel could not be applied.
		outcomes.push_back(
			OrderOutcome::cancel_rejected(id, reject_reason::UNKNOWN_ORDER));
		return;
	}

	const auto [side, price, node] = found->second;
	book_side &levels              = side_levels(side);
	Level *level                   = levels.find(price);
	// An index entry names a resting order, and a resting order's level exists.
	assert(level != nullptr && "index entry outlived its level");
	if (level == nullptr) {
		// Unreachable, but a cancel request resolves exactly once either way —
		// a silent return here is the hole this whole path exists to close.
		index_.erase(found);
		outcomes.push_back(
			OrderOutcome::cancel_rejected(id, reject_reason::UNKNOWN_ORDER));
		return;
	}

	// Cancel the node's state before unlinking so the outcome carries the
	// quantity it executed: a cancellation withdraws the remainder and freezes
	// the rest, it does not undo the fills.
	detail::resting_order &resting = pool_.get(node).value;
	resting.cancel();
	outcomes.push_back(OrderOutcome::cancelled(id, resting.state()));

	// The location carries the node, so this is a splice, not a search.
	level->orders.unlink(pool_, node);
	pool_.deallocate(node);
	if (level->orders.is_empty()) levels.erase(price);
	index_.erase(found);
}

void order_book::cancel_order(order_id_t id) {
	std::vector<OrderOutcome> discarded;
	cancel_order(id, discarded);
}

void order_book::delete_order(side_t side, price_t price, quantity_t volume) {
	book_side &levels = side_levels(side);
	Level *level      = levels.find(price);
	if (level == nullptr) return;

	auto &orders = level->orders;
	while (volume > 0 && !orders.is_empty()) {
		detail::resting_order &head = orders.front(pool_);
		const quantity_t take       = std::min(volume, head.qty());
		orders.reduce_front(pool_, take);
		volume -= take;
		if (!head.has_quantity()) pop_front(*level);
	}
	if (orders.is_empty()) levels.erase(price);
}

quantity_t order_book::volume_at_price(price_t price, side_t side) const {
	return side_levels(side).volume_at_price(price);
}

std::optional<price_t> order_book::best_bid() const {
	return bid_.best_price();
}

std::optional<price_t> order_book::best_ask() const {
	return ask_.best_price();
}

book_side &order_book::side_levels(side_t s) {
	return s == side_t::bid ? bid_ : ask_;
}

const book_side &order_book::side_levels(side_t s) const {
	return s == side_t::bid ? bid_ : ask_;
}

void order_book::pop_front(Level &level) {
	const order_id_t id = level.orders.front(pool_).id();
	if (id != kAnonymous) index_.erase(id);
	pool_.deallocate(level.orders.pop_front(pool_));
}

bool order_book::is_price_crossing(side_t side, price_t price,
								   price_t book_price) {
	// A bid_ crosses an ask priced at or below it; an ask crosses a bid_ priced
	// at or above it.
	return side == side_t::bid ? price >= book_price : price <= book_price;
}

bool order_book::can_fully_fill(const book_side &opposite, side_t side,
								price_t price, quantity_t volume) const {
	quantity_t available = 0;
	for (const Level &level : opposite) {
		if (!is_price_crossing(side, price, level.price)) break;
		available += level.total_volume();
		if (available >= volume) return true;
	}
	return false;
}

} // namespace exchange::engine
