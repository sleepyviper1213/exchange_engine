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
										   price_t price, bool descending) {
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
seek(const std::vector<l2_book::Level> &levels, price_t price, bool descending) {
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

l2_book::l2_book(std::size_t max_depth) : max_depth_(max_depth) {
	// A capped side is never longer than max_depth cells, so reserving that up
	// front is bounded by construction — and it is what keeps set_level off the
	// allocator: with the capacity already in place an insert is a memmove and
	// never a reallocation.
	if (max_depth != UNBOUNDED_DEPTH) {
		bids_.reserve(max_depth);
		asks_.reserve(max_depth);
	}
}

void l2_book::set_level(side_t side, price_t price, quantity_t volume) {
	const bool is_bid          = side == side_t::bid;
	std::vector<Level> &levels = is_bid ? bids_ : asks_;

	const auto at = seek(levels, price, is_bid);
	if (at != levels.end() && at->price == price) {
		// Level exists: overwrite its absolute size, or remove it at size 0.
		if (volume <= 0) levels.erase(at);
		else at->qty = volume;
		return;
	}
	// No level here, and a remove of an already-absent price is a no-op the feed
	// can legitimately send.
	if (volume <= 0) return;

	if (max_depth_ != UNBOUNDED_DEPTH && levels.size() >= max_depth_) {
		// The window is full. A price ordered worse than every level in it is
		// outside the retained view, so it is not kept at all; one that lands
		// inside evicts the current worst level to make room. Evicting is also
		// what bounds the insert: the shift can never exceed max_depth cells.
		if (at == levels.end()) return;
		// `at` addresses an element, so it survives pop_back — as the new end()
		// if it happened to name the evicted level, which is exactly where a
		// price better than only that level belongs.
		levels.pop_back();
	}
	levels.emplace(at, price, volume);
}

void l2_book::load(side_t side, std::vector<Level> levels) {
	const bool is_bid = side == side_t::bid;

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

	// Best-first order is established by now, so a capped side keeps the head of
	// the window and drops the tail — the depth a top-N consumer never reads.
	if (max_depth_ != UNBOUNDED_DEPTH && levels.size() > max_depth_)
		levels.resize(max_depth_);

	std::vector<Level> &side_levels = is_bid ? bids_ : asks_;
	side_levels                     = std::move(levels);
	// Taking the caller's buffer also takes its capacity, which the constructor's
	// reservation no longer covers. Restore it here, once, on the snapshot path,
	// so the update path keeps its no-reallocation guarantee.
	if (max_depth_ != UNBOUNDED_DEPTH) side_levels.reserve(max_depth_);
}

void l2_book::clear() noexcept {
	bids_.clear();
	asks_.clear();
}

std::optional<price_t> l2_book::best_bid() const noexcept {
	if (bids_.empty()) return std::nullopt;
	return bids_.front().price;
}

std::optional<price_t> l2_book::best_ask() const noexcept {
	if (asks_.empty()) return std::nullopt;
	return asks_.front().price;
}

bool l2_book::is_crossed() const noexcept {
	// One side empty is not a cross — it is a book with nothing to cross with.
	if (bids_.empty() || asks_.empty()) return false;
	return bids_.front().price >= asks_.front().price;
}

quantity_t l2_book::volume_at_price(price_t price, side_t side) const {
	const bool is_bid                = side == side_t::bid;
	const std::vector<Level> &levels = is_bid ? bids_ : asks_;

	const auto at = seek(levels, price, is_bid);
	if (at != levels.end() && at->price == price) return at->qty;
	return 0;
}

std::size_t l2_book::depth(side_t side) const noexcept {
	return (side == side_t::bid ? bids_ : asks_).size();
}

std::size_t l2_book::max_depth() const noexcept { return max_depth_; }

} // namespace exchange::market_data
