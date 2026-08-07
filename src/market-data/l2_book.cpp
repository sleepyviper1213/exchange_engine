#include "l2_book.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <ranges>

namespace exchange::market_data {
namespace {

using price_level = l2_book::price_level;

// Sorted insertion point for @p price under a side's ordering: the index of the
// first cell not ordered strictly better than @p price. For a hit the cell at
// the returned index holds @p price; otherwise it is where a new cell belongs,
// which may be @p size. Bids sort descending (better = higher), asks ascending.
[[nodiscard]] std::size_t seek(const price_level *data, std::size_t size,
							   scaled_price_t price, bool descending) noexcept {
	std::size_t low  = 0;
	std::size_t high = size;
	while (low < high) {
		const std::size_t mid = low + ((high - low) / 2);
		const bool better =
			descending ? data[mid].price > price : data[mid].price < price;
		if (better) low = mid + 1;
		else high = mid;
	}
	return low;
}

[[nodiscard]] bool hit(const price_level *data, std::size_t size, std::size_t at,
					   scaled_price_t price) noexcept {
	return at < size && data[at].price == price;
}

} // namespace

l2_book::l2_book(std::size_t max_depth)
	// One block for both sides. Value-initialised because the cells past the
	// live prefix are never read, and paying once at construction to keep them
	// deterministic is cheaper than reasoning about it later.
	: cells_(std::make_unique<price_level[]>(max_depth * 2)), max_depth_(max_depth) {
	assert(max_depth > 0 && "a book with no depth cannot hold a price");
}

void l2_book::set_level(side_t side, scaled_price_t price, scaled_qty_t volume) {
	const bool is_bid = side == side_t::bid;
	price_level *data       = is_bid ? bids() : asks();
	std::size_t &size = is_bid ? bid_size_ : ask_size_;

	const std::size_t at = seek(data, size, price, is_bid);
	if (hit(data, size, at, price)) {
		// Level exists: overwrite its absolute size, or remove it at size 0.
		if (volume <= 0) {
			std::move(data + at + 1, data + size, data + at);
			--size;
		} else {
			data[at].qty = volume;
		}
		return;
	}
	// No level here, and a remove of an already-absent price is a no-op the
	// feed can legitimately send — it may name a level that fell out of the
	// window.
	if (volume <= 0) return;

	if (size == max_depth_) {
		// The window is full, so this price costs another its place either way:
		// one ordered worse than everything retained is outside the view and is
		// not kept at all, and one that lands inside evicts the current worst.
		++dropped_levels_;
		if (at >= size) return;
		--size;
	}
	std::move_backward(data + at, data + size, data + size + 1);
	data[at] = price_level{.price=price, .qty=volume};
	++size;
}

void l2_book::load(side_t side, std::span<const price_level> levels) {
	const bool is_bid = side == side_t::bid;
	price_level *dest       = is_bid ? bids() : asks();
	std::size_t &size = is_bid ? bid_size_ : ask_size_;

	// A non-positive size is the wire's way of spelling "no level here", so it
	// never becomes a cell — and must not occupy a slot a real level wants.
	// Not const: filter_view caches its first match, so begin() is non-const
	// and a const filter_view does not model range at all.
	auto positive = levels | std::views::filter([](const price_level &level) {
						return level.qty > 0;
					});

	// Selection, not sort-then-truncate: partial_sort_copy walks the input once
	// and writes only the best max_depth_ cells, in order, into storage that
	// already exists. Sorting instead would need a mutable copy of the caller's
	// levels, which is the allocation this book exists to avoid.
	const std::span<price_level> window{dest, max_depth_};
	const auto copied = is_bid
							? std::ranges::partial_sort_copy(positive,
															 window,
															 std::greater<>{},
															 &price_level::price,
															 &price_level::price)
							: std::ranges::partial_sort_copy(positive,
															 window,
															 std::less<>{},
															 &price_level::price,
															 &price_level::price);
	// Against window.begin(), not dest: the result iterator is the span's,
	// which is not a raw pointer under a hardened standard library.
	const auto written = static_cast<std::size_t>(copied.out - window.begin());

	// A duplicated price would break the binary search every other operation
	// relies on; a well-formed snapshot has none, and the first wins if one
	// ever does. Duplicates are adjacent now that the window is sorted.
	const auto surplus =
		std::ranges::unique(window.first(written), {}, &price_level::price);
	size = written - static_cast<std::size_t>(std::ranges::distance(surplus));

	// Only depth the window could not hold is a drop; a duplicate price was
	// never a distinct level to begin with.
	const auto offered =
		static_cast<std::size_t>(std::ranges::distance(positive));
	if (offered > written) dropped_levels_ += offered - written;
}

void l2_book::clear() noexcept {
	bid_size_ = 0;
	ask_size_ = 0;
}

std::optional<scaled_price_t> l2_book::best_bid() const noexcept {
	if (bid_size_ == 0) return std::nullopt;
	return bids()[0].price;
}

std::optional<scaled_price_t> l2_book::best_ask() const noexcept {
	if (ask_size_ == 0) return std::nullopt;
	return asks()[0].price;
}

bool l2_book::is_crossed() const noexcept {
	// One side empty is not a cross — it is a book with nothing to cross with.
	if (bid_size_ == 0 || ask_size_ == 0) return false;
	return bids()[0].price >= asks()[0].price;
}

scaled_qty_t l2_book::volume_at_price(scaled_price_t price, side_t side) const {
	const bool is_bid   = side == side_t::bid;
	const price_level *data   = is_bid ? bids() : asks();
	const std::size_t n = is_bid ? bid_size_ : ask_size_;

	const std::size_t at = seek(data, n, price, is_bid);
	return hit(data, n, at, price) ? data[at].qty : 0;
}

std::size_t l2_book::depth(side_t side) const noexcept {
	return side == side_t::bid ? bid_size_ : ask_size_;
}

std::size_t l2_book::max_depth() const noexcept { return max_depth_; }

std::uint64_t l2_book::dropped_levels() const noexcept {
	return dropped_levels_;
}

[[nodiscard]] std::span<const price_level> l2_book::bid_levels() const noexcept {
	return {bids(), bid_size_};
}

[[nodiscard]] std::span<const price_level> l2_book::ask_levels() const noexcept {
	return {asks(), ask_size_};
}

std::size_t l2_book::size() const noexcept { return bid_size_ + ask_size_; }

bool l2_book::is_empty() const noexcept { return size() == 0; }

[[nodiscard]] l2_book::price_level *l2_book::bids() noexcept { return cells_.get(); }

[[nodiscard]] l2_book::price_level *l2_book::asks() noexcept {
	return cells_.get() + max_depth_;
}

[[nodiscard]] const l2_book::price_level *l2_book::bids() const noexcept {
	return cells_.get();
}

[[nodiscard]] const l2_book::price_level *l2_book::asks() const noexcept {
	return cells_.get() + max_depth_;
}


} // namespace exchange::market_data
