#include "book_side.hpp"

#include "../level.hpp"
#include "core/optimisation/branchless_binary_search.hpp"

#include <cassert>
#include <functional>

namespace exchange::engine {
using core::optimisation::branchless_lower_bound;
using detail::book_side;

book_side::book_side(side side, detail::order_pool &pool) noexcept
	: side_(side), pool_(pool) {}

bool book_side::empty() const noexcept { return levels_.empty(); }

std::optional<price> book_side::best_price() const {
	if (levels_.empty()) return std::nullopt;
	return levels_.front().price;
}

Level &book_side::best() { return levels_.front(); }

const Level &book_side::best() const { return levels_.front(); }

std::vector<Level>::iterator book_side::lower_bound(price p) {
	return side_ == side::bid ? branchless_lower_bound(levels_,
													   p,
													   std::greater<price>{},
													   &Level::price)
							  : branchless_lower_bound(levels_,
													   p,
													   std::less<price>{},
													   &Level::price);
}

std::vector<Level>::const_iterator book_side::lower_bound(price p) const {
	return side_ == side::bid ? branchless_lower_bound(levels_,
													   p,
													   std::greater<price>{},
													   &Level::price)
							  : branchless_lower_bound(levels_,
													   p,
													   std::less<price>{},
													   &Level::price);
}

Level *book_side::find(price p) {
	const auto it = lower_bound(p);
	return it != levels_.end() && it->price == p ? &*it : nullptr;
}

const Level *book_side::find(price p) const {
	const auto it = lower_bound(p);
	return it != levels_.end() && it->price == p ? &*it : nullptr;
}

Level &book_side::insert(const Order &incoming) {
	const auto it = lower_bound(incoming.price);
	// Creating the level first, then resting the order into it, keeps the one
	// pool allocation on a single path: the vector shift below moves plain
	// scalars, so it must not run while a node index is in flight.
	Level &level = it != levels_.end() && it->price == incoming.price
		               ? *it
		               : *levels_.emplace(
			                 it,
			                 Level{.price = incoming.price, .orders = {}});
	level.add_order(pool_, incoming);
	return level;
}

void book_side::remove_best_level_if_empty() {
	assert(!levels_.empty());
	if (levels_.front().has_empty_orders()) levels_.erase(levels_.begin());
}

void book_side::erase(price p) {
	const auto it = lower_bound(p);
	if (it == levels_.end() || it->price != p) return;
	// The level owns pool slots, not memory: dropping the cell without draining
	// its FIFO first would strand every node still on it.
	release_nodes(*it);
	levels_.erase(it);
}

void book_side::release_nodes(Level &level) {
	while (!level.orders.is_empty())
		pool_.deallocate(level.orders.pop_front(pool_));
}

quantity book_side::volume_at_price(price p) const {
	const Level *level = find(p);
	return level != nullptr ? level->total_volume() : 0;
}

std::vector<Level>::const_iterator book_side::begin() const noexcept {
	return levels_.begin();
}

std::vector<Level>::const_iterator book_side::end() const noexcept {
	return levels_.end();
}

} // namespace exchange::engine
