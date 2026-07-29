#include "l2_book.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace exchange::market_data {
namespace {

// Sorted insertion point for @p price under a side's ordering: the first level
// not ordered strictly better than @p price. For a hit, the returned iterator's
// price equals @p price; otherwise it is where a new level belongs. Bids sort
// descending (better = higher), asks ascending (better = lower).
std::vector<l2_book::Level>::iterator seek(std::vector<l2_book::Level> &levels,
										   price price, bool descending) {
	if (descending)
		return std::ranges::lower_bound(levels,
										price,
										std::greater<>{},
										&l2_book::Level::price);

	return std::ranges::lower_bound(levels,
									price,
									std::less<>{},
									&l2_book::Level::price);
}

std::vector<l2_book::Level>::const_iterator
seek(const std::vector<l2_book::Level> &levels, price price, bool descending) {
	if (descending)
		return std::ranges::lower_bound(levels,
										price,
										std::greater<>{},
										&l2_book::Level::price);

	return std::ranges::lower_bound(levels,
									price,
									std::less<>{},
									&l2_book::Level::price);
}

} // namespace

void l2_book::set_level(side side, price price, quantity volume) {
	const bool is_bid          = side == side::bid;
	std::vector<Level> &levels = is_bid ? bids_ : asks_;

	const auto at = seek(levels, price, is_bid);
	if (at != levels.end() && at->price == price) {
		// Level exists: overwrite its absolute size, or remove it at size 0.
		if (volume <= 0) levels.erase(at);
		else at->qty = volume;
		return;
	}
	// No level here: create one in sorted position, unless it is a remove of an
	// already-absent price (a no-op the feed can legitimately send).
	if (volume > 0) levels.emplace(at, price, volume);
}

void l2_book::load(side side, std::vector<Level> levels) {
	const bool is_bid = side == side::bid;

	// A non-positive size is the wire's way of spelling "no level here", so it
	// never becomes a cell.
	std::erase_if(levels, [](const Level &level) { return level.qty <= 0; });

	if (is_bid)
		std::ranges::sort(levels, std::greater<>{}, &Level::price);
	else std::ranges::sort(levels, std::less<>{}, &Level::price);

	// A duplicated price would break the binary search set_level relies on; a
	// well-formed snapshot has none, and the first wins if one ever does.
	const auto duplicates = std::ranges::unique(levels, {}, &Level::price);
	levels.erase(duplicates.begin(), duplicates.end());

	(is_bid ? bids_ : asks_) = std::move(levels);
}

void l2_book::clear() noexcept {
	bids_.clear();
	asks_.clear();
}

std::optional<price> l2_book::best_bid() const noexcept {
	if (bids_.empty()) return std::nullopt;
	return bids_.front().price;
}

std::optional<price> l2_book::best_ask() const noexcept {
	if (asks_.empty()) return std::nullopt;
	return asks_.front().price;
}

quantity l2_book::volume_at_price(price price, side side) const {
	const bool is_bid                = side == side::bid;
	const std::vector<Level> &levels = is_bid ? bids_ : asks_;

	const auto at = seek(levels, price, is_bid);
	if (at != levels.end() && at->price == price) return at->qty;
	return 0;
}

std::size_t l2_book::depth(side side) const noexcept {
	return (side == side::bid ? bids_ : asks_).size();
}

const std::vector<l2_book::Level> &l2_book::levels(side side) const noexcept {
	return side == side::bid ? bids_ : asks_;
}

} // namespace exchange::market_data
