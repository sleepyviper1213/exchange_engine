#include "book_side.hpp"

#include "../level.hpp"
#include "core/optimisation/branchless_binary_search.hpp"

#include <cassert>
#include <functional>

namespace exchange::engine {
using core::optimisation::branchless_lower_bound;
using detail::book_side;

book_side::book_side(side_t side, detail::order_pool &pool) noexcept
	: side_(side), pool_(pool) {}

bool book_side::empty() const noexcept { return levels_.empty(); }

std::optional<price_t> book_side::best_price() const {
	if (levels_.empty()) return std::nullopt;
	return levels_.front().price;
}

Level &book_side::best() { return levels_.front(); }

const Level &book_side::best() const { return levels_.front(); }

std::vector<Level>::iterator book_side::lower_bound(price_t price) {
	return side_ == side_t::bid ? branchless_lower_bound(levels_,
													   price,
													   std::greater<price_t>{},
													   &Level::price)
							  : branchless_lower_bound(levels_,
													   price,
													   std::less<price_t>{},
													   &Level::price);
}

std::vector<Level>::const_iterator book_side::lower_bound(price_t price) const {
	return side_ == side_t::bid ? branchless_lower_bound(levels_,
													   price,
													   std::greater<price_t>{},
													   &Level::price)
							  : branchless_lower_bound(levels_,
													   price,
													   std::less<price_t>{},
													   &Level::price);
}

Level *book_side::find(price_t price) {
	const auto it = lower_bound(price);
	return it != levels_.end() && it->price == price ? &*it : nullptr;
}

const Level *book_side::find(price_t price) const {
	const auto it = lower_bound(price);
	return it != levels_.end() && it->price == price ? &*it : nullptr;
}

Level &book_side::level_at(price_t price) {
	const auto it = lower_bound(price);
	// Creating the level first, then resting the order into it, keeps the one
	// pool allocation on a single path: the vector shift here moves plain
	// scalars, so it must not run while a node index is in flight.
	return it != levels_.end() && it->price == price
			   ? *it
			   : *levels_.emplace(it, Level{.price = price, .orders = {}});
}

Level &book_side::insert(const Order &incoming) {
	Level &level = level_at(incoming.price);
	level.add_order(pool_, incoming);
	return level;
}

Level &book_side::insert(order_id_t id, price_t price,
						 const order_state &state) {
	Level &level = level_at(price);
	level.add_order(pool_, id, state);
	return level;
}

void book_side::remove_best_level_if_empty() {
	assert(!levels_.empty());
	if (levels_.front().has_empty_orders()) levels_.erase(levels_.begin());
}

void book_side::erase(price_t price) {
	const auto it = lower_bound(price);
	if (it == levels_.end() || it->price != price) return;
	// The level owns pool slots, not memory: dropping the cell without draining
	// its FIFO first would strand every node still on it.
	release_nodes(*it);
	levels_.erase(it);
}

void book_side::release_nodes(Level &level) {
	while (!level.orders.is_empty())
		pool_.deallocate(level.orders.pop_front(pool_));
}

quantity_t book_side::volume_at_price(price_t price) const {
	const Level *level = find(price);
	return level != nullptr ? level->total_volume() : 0;
}

std::vector<Level>::const_iterator book_side::begin() const noexcept {
	return levels_.begin();
}

std::vector<Level>::const_iterator book_side::end() const noexcept {
	return levels_.end();
}

} // namespace exchange::engine
