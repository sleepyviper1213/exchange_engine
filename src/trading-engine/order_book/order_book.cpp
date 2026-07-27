#include "order_book.hpp"

#include "core/types.hpp"
#include "detail/book_side.hpp"
#include "level.hpp"
#include "order.hpp"
#include "trade.hpp"

#include <algorithm>

namespace exchange::engine {

using detail::book_side;

order_book::order_book(std::size_t /*capacity*/)
	: bid_(Side::BID), ask_(Side::ASK) {}

void order_book::place_order(const Order &incoming, std::vector<Trade> &out) {
	book_side &opposite = side_levels(opposed(incoming.side));

	// Fill-or-kill is all-or-nothing: if the resting liquidity cannot fully
	// fill the order right now, execute nothing and leave the book untouched.
	if (incoming.type == OrderType::FILL_OR_KILL &&
		!can_fully_fill(opposite,
						incoming.side,
						incoming.price,
						incoming.volume))
		return;

	Order remaining = incoming; // mutable working copy; decremented as it fills

	while (remaining.has_quantity() && !opposite.empty()) {
		Level &best = opposite.best();
		if (!is_price_crossing(remaining, best.price)) break;

		while (remaining.has_quantity() && !best.has_empty_orders()) {
			Order &resting      = best.orders.front();
			const Volume traded = std::min(remaining.volume, resting.volume);
			// todo: Notify trade events here
			out.emplace_back(remaining.id, resting.id, best.price, traded);
			remaining.decrease_volume_by(traded);
			resting.decrease_volume_by(traded);

			if (!resting.has_quantity()) pop_front(best);
		}
		opposite.remove_best_level_if_empty();
	}

	// Only GTC rests a remainder; IOC (and a partially-filled FOK, which cannot
	// happen given the pre-check) drop whatever did not cross.
	if (remaining.has_quantity() &&
		remaining.type == OrderType::GOOD_TILL_CANCELLED) {
		book_side &own = side_levels(remaining.side);
		(void)own.insert(remaining);
		if (remaining.id != kAnonymous)
			index_[remaining.id] = Location{remaining.side, remaining.price};
	}
}

std::vector<Trade> order_book::place_order(const Order &incoming) {
	std::vector<Trade> trades;
	place_order(incoming, trades);
	return trades;
}

void order_book::add_order(Side side, Price price, Volume volume) {
	// Anonymous resting liquidity: no id (untracked for cancel), no matching.
	side_levels(side).insert(Order{.id     = kAnonymous,
								   .side   = side,
								   .price  = price,
								   .volume = volume});
}

void order_book::cancel_order(OrderId id) {
	const auto found = index_.find(id);
	if (found == index_.end()) return;

	const auto [side, price] = found->second;
	book_side &levels        = side_levels(side);
	if (Level *level = levels.find(price); level != nullptr) {
		auto &orders     = level->orders;
		const auto order = std::ranges::find(orders, id, &Order::id);
		if (order != orders.end()) orders.erase(order);
		if (level->orders.empty()) levels.erase(price);
	}
	index_.erase(found);
}

void order_book::delete_order(Side side, Price price, Volume volume) {
	book_side &levels = side_levels(side);
	Level *level      = levels.find(price);
	if (level == nullptr) return;

	auto &orders = level->orders;
	while (volume > 0 && !orders.empty()) {
		Order &head       = orders.front();
		const Volume take = std::min(volume, head.volume);
		head.decrease_volume_by(take);
		volume -= take;
		if (!head.has_quantity()) pop_front(*level);
	}
	if (level->orders.empty()) levels.erase(price);
}

void order_book::set_level(Side side, Price price, Volume volume) {
	book_side &levels = side_levels(side);

	if (volume <= 0) {
		// absolute size 0 (or negative) means "remove this price".
		levels.erase(price);
		return;
	}

	// L2 diff feed: the level is a single anonymous node carrying the
	// aggregate. insert() places the level by the order's price, so it must
	// carry the real price (a default {} would create the level at price 0).
	Level &level = levels.insert(Order{.id     = kAnonymous,
									   .side   = side,
									   .price  = price,
									   .volume = volume});
	level.orders.clear();
	level.orders.emplace_back(kAnonymous, side, price, volume);
}

Volume order_book::volume_at_price(Price price, Side side) const {
	return side_levels(side).volume_at_price(price);
}

std::optional<Price> order_book::best_bid() const { return bid_.best_price(); }

std::optional<Price> order_book::best_ask() const { return ask_.best_price(); }

book_side &order_book::side_levels(Side s) {
	return s == Side::BID ? bid_ : ask_;
}

const book_side &order_book::side_levels(Side s) const {
	return s == Side::BID ? bid_ : ask_;
}

void order_book::pop_front(Level &level) {
	const Order &head = level.orders.front();
	if (head.id != kAnonymous) index_.erase(head.id);
	level.orders.erase(level.orders.begin());
}

bool order_book::is_price_crossing(const Order &incoming, Price book_price) {
	return is_price_crossing(incoming.side, incoming.price, book_price);
}

bool order_book::is_price_crossing(Side side, Price price, Price book_price) {
	// A bid_ crosses an ask priced at or below it; an ask crosses a bid_ priced
	// at or above it.
	return side == Side::BID ? price >= book_price : price <= book_price;
}

bool order_book::can_fully_fill(const book_side &opposite, Side side,
								Price price, Volume volume) const {
	Volume available = 0;
	for (const Level &level : opposite) {
		if (!is_price_crossing(side, price, level.price)) break;
		available += level.total_volume();
		if (available >= volume) return true;
	}
	return false;
}

} // namespace exchange::engine
