#include "l2_book.hpp"

#include <algorithm>

namespace exchange::market_data {
namespace {

// Sorted insertion point for @p price under a side's ordering: the first level
// not ordered strictly better than @p price. For a hit, the returned iterator's
// price equals @p price; otherwise it is where a new level belongs. Bids sort
// descending (better = higher), asks ascending (better = lower).
std::vector<l2_book::Level>::iterator seek(std::vector<l2_book::Level> &levels,
										   Price price, bool descending) {
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
seek(const std::vector<l2_book::Level> &levels, Price price, bool descending) {
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

void l2_book::set_level(Side side, Price price, Volume volume) {
	const bool is_bid          = side == Side::BID;
	std::vector<Level> &levels = is_bid ? bids_ : asks_;

	const auto at = seek(levels, price, is_bid);
	if (at != levels.end() && at->price == price) {
		// Level exists: overwrite its absolute size, or remove it at size 0.
		if (volume <= 0) levels.erase(at);
		else at->volume = volume;
		return;
	}
	// No level here: create one in sorted position, unless it is a remove of an
	// already-absent price (a no-op the feed can legitimately send).
	if (volume > 0) levels.emplace(at, price, volume);
}

void l2_book::clear() noexcept {
	bids_.clear();
	asks_.clear();
}

std::optional<Price> l2_book::best_bid() const noexcept {
	if (bids_.empty()) return std::nullopt;
	return bids_.front().price;
}

std::optional<Price> l2_book::best_ask() const noexcept {
	if (asks_.empty()) return std::nullopt;
	return asks_.front().price;
}

Volume l2_book::volume_at_price(Price price, Side side) const {
	const bool is_bid                = side == Side::BID;
	const std::vector<Level> &levels = is_bid ? bids_ : asks_;

	const auto at = seek(levels, price, is_bid);
	if (at != levels.end() && at->price == price) return at->volume;
	return 0;
}

std::size_t l2_book::depth(Side side) const noexcept {
	return (side == Side::BID ? bids_ : asks_).size();
}

const std::vector<l2_book::Level> &l2_book::levels(Side side) const noexcept {
	return side == Side::BID ? bids_ : asks_;
}

} // namespace exchange::market_data
