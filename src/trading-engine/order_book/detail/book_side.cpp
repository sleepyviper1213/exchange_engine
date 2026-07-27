#include "book_side.hpp"

#include "../level.hpp"
#include "core/optimisation/branchless_binary_search.hpp"

#include <cassert>
#include <functional>

namespace exchange::engine {
using detail::book_side;
using core::optimisation::branchless_lower_bound;

book_side::book_side(Side side) noexcept : side_(side) {}

bool book_side::empty() const noexcept { return levels_.empty(); }

std::optional<Price> book_side::best_price() const {
	if (levels_.empty()) return std::nullopt;
	return levels_.front().price;
}

Level &book_side::best() { return levels_.front(); }

const Level &book_side::best() const { return levels_.front(); }

std::vector<Level>::iterator book_side::lower_bound(Price price) {
	return side_ == Side::BID ? branchless_lower_bound(levels_,
													   price,
													   std::greater<Price>{},
													   &Level::price)
							  : branchless_lower_bound(levels_,
													   price,
													   std::less<Price>{},
													   &Level::price);
}

std::vector<Level>::const_iterator book_side::lower_bound(Price price) const {
	return side_ == Side::BID ? branchless_lower_bound(levels_,
													   price,
													   std::greater<Price>{},
													   &Level::price)
							  : branchless_lower_bound(levels_,
													   price,
													   std::less<Price>{},
													   &Level::price);
}

Level *book_side::find(Price price) {
	const auto it = lower_bound(price);
	return it != levels_.end() && it->price == price ? &*it : nullptr;
}

const Level *book_side::find(Price price) const {
	const auto it = lower_bound(price);
	return it != levels_.end() && it->price == price ? &*it : nullptr;
}

Level &book_side::insert(const Order &incoming) {
	const auto it = lower_bound(incoming.price);
	if (it != levels_.end() && it->price == incoming.price) {
		it->orders.push_back(incoming);
		return *it;
	}
	return *levels_.emplace(
		it,
		Level{.price = incoming.price, .orders = {incoming}});
}

void book_side::remove_best_level_if_empty() {
	assert(!levels_.empty());
	if (levels_.front().has_empty_orders()) levels_.erase(levels_.begin());
}

void book_side::erase(Price price) {
	const auto it = lower_bound(price);
	if (it != levels_.end() && it->price == price) levels_.erase(it);
}

Volume book_side::volume_at_price(Price price) const {
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
