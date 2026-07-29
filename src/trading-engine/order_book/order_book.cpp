#include "order_book.hpp"

#include "core/types.hpp"
#include "detail/book_side.hpp"
#include "level.hpp"
#include "order.hpp"
#include "trade.hpp"

#include <algorithm>

namespace exchange::engine {

using detail::book_side;

order_book::order_book(std::size_t capacity)
	: pool_(capacity), bid_(side::bid, pool_), ask_(side::ask, pool_) {
	index_.reserve(capacity);
}

void order_book::place_order(const Order &incoming, std::vector<Trade> &out) {
	book_side &opposite = side_levels(opposed(incoming.side));

	// Fill-or-kill is all-or-nothing: if the resting liquidity cannot fully
	// fill the order right now, execute nothing and leave the book untouched.
	if (incoming.type == OrderType::FILL_OR_KILL &&
		!can_fully_fill(opposite,
						incoming.side,
						incoming.price,
						incoming.qty))
		return;

	Order remaining = incoming; // mutable working copy; decremented as it fills

	while (remaining.has_quantity() && !opposite.empty()) {
		Level &best = opposite.best();
		if (!is_price_crossing(remaining, best.price)) break;

		while (remaining.has_quantity() && !best.has_empty_orders()) {
			detail::resting_order &resting = best.orders.front(pool_);
			const quantity traded = std::min(remaining.qty, resting.qty());
			// todo: Notify trade events here
			out.emplace_back(remaining.id, resting.id(), best.price, traded);
			remaining.decrease_volume_by(traded);
			// Goes through the list so the level's cached aggregate tracks the
			// fill; the reference stays valid, it is the same node.
			best.orders.reduce_front(pool_, traded);

			if (!resting.has_quantity()) pop_front(best);
		}
		opposite.remove_best_level_if_empty();
	}

	// Only GTC rests a remainder; IOC (and a partially-filled FOK, which cannot
	// happen given the pre-check) drop whatever did not cross.
	if (remaining.has_quantity() &&
		remaining.type == OrderType::GOOD_TILL_CANCELLED) {
		book_side &own     = side_levels(remaining.side);
		const Level &level = own.insert(remaining);
		if (remaining.id != kAnonymous)
			index_[remaining.id] =
				Location{remaining.side, remaining.price, level.orders.back()};
	}
}

std::vector<Trade> order_book::place_order(const Order &incoming) {
	std::vector<Trade> trades;
	place_order(incoming, trades);
	return trades;
}

void order_book::add_order(side side, price price, quantity volume) {
	// Anonymous resting liquidity: no id (untracked for cancel), no matching.
	side_levels(side).insert(Order{.id     = kAnonymous,
								   .side   = side,
								   .price  = price,
								   .qty = volume});
}

void order_book::cancel_order(order_id id) {
	const auto found = index_.find(id);
	if (found == index_.end()) return;

	const auto [side, price, node] = found->second;
	book_side &levels              = side_levels(side);
	if (Level *level = levels.find(price); level != nullptr) {
		// The location carries the node, so this is a splice, not a search.
		level->orders.unlink(pool_, node);
		pool_.deallocate(node);
		if (level->orders.is_empty()) levels.erase(price);
	}
	index_.erase(found);
}

void order_book::delete_order(side side, price price, quantity volume) {
	book_side &levels = side_levels(side);
	Level *level      = levels.find(price);
	if (level == nullptr) return;

	auto &orders = level->orders;
	while (volume > 0 && !orders.is_empty()) {
		detail::resting_order &head = orders.front(pool_);
		const quantity take         = std::min(volume, head.qty());
		orders.reduce_front(pool_, take);
		volume -= take;
		if (!head.has_quantity()) pop_front(*level);
	}
	if (orders.is_empty()) levels.erase(price);
}

void order_book::set_level(side side, price price, quantity volume) {
	book_side &levels = side_levels(side);

	if (volume <= 0) {
		// absolute size 0 (or negative) means "remove this price".
		levels.erase(price);
		return;
	}

	// L2 diff feed: the level is a single anonymous node carrying the
	// aggregate. insert() places the level by the order's price, so it must
	// carry the real price (a default {} would create the level at price 0).
	Level &level = levels.insert(Order{.id    = kAnonymous,
									   .side  = side,
									   .price = price,
									   .qty   = volume});
	// insert() appended a node; collapse the level onto its head so the level
	// carries exactly the absolute size the feed just published, whatever it
	// held before. Freeing the surplus nodes is reset_to_single's job.
	level.orders.reset_to_single(pool_, volume);
}

quantity order_book::volume_at_price(price price, side side) const {
	return side_levels(side).volume_at_price(price);
}

std::optional<price> order_book::best_bid() const { return bid_.best_price(); }

std::optional<price> order_book::best_ask() const { return ask_.best_price(); }

book_side &order_book::side_levels(side s) {
	return s == side::bid ? bid_ : ask_;
}

const book_side &order_book::side_levels(side s) const {
	return s == side::bid ? bid_ : ask_;
}

void order_book::pop_front(Level &level) {
	const order_id id = level.orders.front(pool_).id();
	if (id != kAnonymous) index_.erase(id);
	pool_.deallocate(level.orders.pop_front(pool_));
}

bool order_book::is_price_crossing(const Order &incoming, price book_price) {
	return is_price_crossing(incoming.side, incoming.price, book_price);
}

bool order_book::is_price_crossing(side side, price p, price book_price) {
	// A bid_ crosses an ask priced at or below it; an ask crosses a bid_ priced
	// at or above it.
	return side == side::bid ? p >= book_price : p <= book_price;
}

bool order_book::can_fully_fill(const book_side &opposite, side side,
								price price, quantity volume) const {
	quantity available = 0;
	for (const Level &level : opposite) {
		if (!is_price_crossing(side, price, level.price)) break;
		available += level.total_volume();
		if (available >= volume) return true;
	}
	return false;
}

} // namespace exchange::engine
