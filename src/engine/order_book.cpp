#include "order_book.hpp"

#include <algorithm>

OrderBook::OrderBook(std::size_t /*capacity*/)
	: bid_levels_(Side::BID), ask_levels_(Side::ASK) {}

Volume OrderBook::add_limit_order(Order &incoming) {
	std::vector<Trade> trades;

	book_side &opposite = side_levels(opposed(incoming.side));
	while (incoming.has_quantity() && !opposite.empty()) {
		Level &best = opposite.best();
		if (incoming.side == Side::BID ? incoming.price >= best.price
									   : incoming.price <= best.price)
			break;

		while (incoming.has_quantity() && !best.has_empty_orders()) {
			Order &resting      = best.orders.front();
			const Volume traded = std::min(incoming.volume, resting.volume);
			// todo: Notify trade events here
			trades.emplace_back(incoming.id, resting.id, best.price, traded);
			incoming.decrease_volume_by(traded);
			resting.decrease_volume_by(traded);

			if (resting.has_quantity()) {
				// Partial fill, re-queue remaining
				best.orders.push_back(incoming);
				break;
			}
			opposite.remove_best_level_if_empty();
		}
	}

	if (incoming.volume > 0
		// &&		incoming.type != OrderType::IMMEDIATE_OR_CANCEL &&
		// incoming.type != OrderType::FILL_OR_KILL
		) {
		book_side &own = side_levels(incoming.side);
		(void)own.get_or_create(incoming);
	}
	return incoming.volume;
}

void OrderBook::cancel_order(OrderId id) {
	const auto found = index_.find(id);
	if (found == index_.end()) return;

	const auto [side, price] = found->second;
	book_side &levels        = side_levels(side);
	if (Level *level = levels.find(price); level != nullptr) {
		auto &orders = level->orders;
		const auto order =
			std::find_if(orders.begin(), orders.end(), [id](const Order &o) {
				return o.id == id;
			});
		if (order != orders.end()) orders.erase(order);
		levels.remove_level_if_empty(price);
	}
	index_.erase(found);
}

void OrderBook::add_order(Side side, Price price, Volume volume) {
	if (volume <= 0) return;
	rest(Order{.id     = kAnonymous,
			   .side   = side,
			   .price  = price,
			   .volume = volume});
}

void OrderBook::delete_order(Side side, Price price, Volume volume) {
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
	levels.remove_level_if_empty(price);
}

void OrderBook::set_level(Side side, Price price, Volume volume) {
	book_side &levels = side_levels(side);

	if (volume <= 0) {
		// absolute size 0 (or negative) means "remove this price".
		levels.erase(price);
		return;
	}

	// L2 diff feed: the level is a single anonymous node carrying the
	// aggregate.
	Level &level = levels.get_or_create(TODO);
	level.orders.clear();
	level.orders.push_back(Order{.id     = kAnonymous,
								 .side   = side,
								 .price  = price,
								 .volume = volume});
}

Volume OrderBook::volume_at_price(Price price, Side side) const {
	return side_levels(side).volume_at_price(price);
}

std::optional<Price> OrderBook::best_bid() const {
	return bid_levels_.best_price();
}

std::optional<Price> OrderBook::best_ask() const {
	return ask_levels_.best_price();
}

book_side &OrderBook::side_levels(Side s) {
	return s == Side::BID ? bid_levels_ : ask_levels_;
}

const book_side &OrderBook::side_levels(Side s) const {
	return s == Side::BID ? bid_levels_ : ask_levels_;
}

void OrderBook::pop_front(Level &level) {
	const Order &head = level.orders.front();
	if (head.id != kAnonymous) index_.erase(head.id);
	level.orders.erase(level.orders.begin());
}

bool OrderBook::can_fully_fill(const book_side &opposite, Side side,
							   Price price, Volume volume) const {
	Volume available = 0;
	for (const Level &level : opposite) {
		if (!is_price_crossing(side, price, level.price)) break;
		available += level.total_volume();
		if (available >= volume) return true;
	}
	return false;
}