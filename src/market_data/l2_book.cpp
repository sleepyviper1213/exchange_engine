#include "l2_book.hpp"

#include "core/simd/ladder.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <type_traits>

namespace exchange::market_data {
namespace {

using price_level = l2_book::price_level;

// Which way a side's prices improve, in one place: bids sort descending (better
// = higher), asks ascending. Everything that needs the ordering - the binary
// search below, load's comparator - asks here rather than testing side_t
// itself, so the rule has exactly one definition to get wrong.
[[nodiscard]] constexpr bool is_descending(side_t side) noexcept {
	return side == side_t::bid;
}

// Sorted insertion point for @p price under @p side's ordering: the index of
// the first cell not ordered strictly better than @p price. For a hit the cell
// at the returned index holds @p price; otherwise it is where a new cell
// belongs, which may be @c levels.size().
[[nodiscard]] std::size_t seek(std::span<const price_level> levels,
							   scaled_price_t price, side_t side) noexcept {
	const bool descending = is_descending(side);
	std::size_t low       = 0;
	std::size_t high      = levels.size();
	while (low < high) {
		const std::size_t mid = low + ((high - low) / 2);
		const bool better =
			descending ? levels[mid].price > price : levels[mid].price < price;
		if (better) low = mid + 1;
		else high = mid;
	}
	return low;
}

[[nodiscard]] bool hit(std::span<const price_level> levels, std::size_t at,
					   scaled_price_t price) noexcept {
	return at < levels.size() && levels[at].price == price;
}

// The cells, viewed as the flat {price, qty, price, qty, ...} array they
// already are, which is the shape core::simd's de-interleaving kernels take.
//
// The cast is checked rather than assumed: the static_asserts below are the
// entire set of facts it relies on, and any edit to price_level that would
// invalidate it - a third field, a narrower price, a reordering - fails the
// build here rather than producing a book that sums the wrong halves. There is
// no object being created, only a read over storage whose layout is asserted,
// which is why this is not a std::start_lifetime_as case.
[[nodiscard]] std::span<const std::int64_t>
as_pairs(std::span<const price_level> levels) noexcept {
	static_assert(std::is_standard_layout_v<price_level>);
	static_assert(std::is_same_v<decltype(price_level::price), std::int64_t>);
	static_assert(std::is_same_v<decltype(price_level::qty), std::int64_t>);
	static_assert(sizeof(price_level) == 2 * sizeof(std::int64_t));
	static_assert(offsetof(price_level, price) == 0);
	static_assert(offsetof(price_level, qty) == sizeof(std::int64_t));

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
	return {reinterpret_cast<const std::int64_t *>(levels.data()),
			levels.size() * 2};
}

} // namespace

l2_book::l2_book(std::size_t max_depth)
	// One block for both sides. Value-initialised because the cells past the
	// live prefix are never read, and paying once at construction to keep them
	// deterministic is cheaper than reasoning about it later.
	: cells_(std::make_unique<price_level[]>(max_depth * 2)),
	  max_depth_(max_depth) {
	assert(max_depth > 0 && "a book with no depth cannot hold a price");
}

void l2_book::set_level(side_t side, scaled_price_t price,
						scaled_qty_t volume) {
	const side_view s                 = mutable_side(side);
	const std::span<price_level> live = s.live();

	const std::size_t at = seek(live, price, side);
	if (hit(live, at, price)) {
		// Level exists: overwrite its absolute size, or remove it at size 0.
		if (volume <= 0) {
			const std::span<price_level> tail = live.subspan(at);
#if __cpp_lib_shift == 202'202L
			std::ranges::shift_left(tail, 1);
#else
			std::shift_left(tail.begin(), tail.end(), 1);
#endif
			--s.size;
		} else {
			live[at].qty = volume;
		}
		return;
	}
	// No level here, and a remove of an already-absent price is a no-op the
	// feed can legitimately send - it may name a level that fell out of the
	// window.
	if (volume <= 0) return;

	if (s.size == s.block.size()) {
		// The window is full, so this price costs another its place either way:
		// one ordered worse than everything retained is outside the view and is
		// not kept at all, and one that lands inside evicts the current worst.
		++dropped_levels_;
		if (at >= s.size) return;
		--s.size;
	}
	// One cell past the live prefix is inside the block: the side is short of
	// max_depth_ here, either because it always was or because the eviction
	// above just made it so.
	const std::span<price_level> hole = s.block.first(s.size + 1).subspan(at);
	std::shift_right(hole.begin(), hole.end(), 1);
	hole.front() = price_level{.price = price, .qty = volume};
	++s.size;
}

void l2_book::load(side_t side, std::span<const price_level> levels) {
	const side_view s = mutable_side(side);

	// A non-positive size is the wire's way of spelling "no level here", so it
	// never becomes a cell - and must not occupy a slot a real level wants.
	// Not const: filter_view caches its first match, so begin() is non-const
	// and a const filter_view does not model range at all.
	auto positive = levels | std::views::filter([](const price_level &level) {
						return level.qty > 0;
					});

	// Selection, not sort-then-truncate: partial_sort_copy walks the input once
	// and writes only the best max_depth_ cells, in order, into storage that
	// already exists. Sorting instead would need a mutable copy of the caller's
	// levels, which is the allocation this book exists to avoid.
	const std::span<price_level> window = s.block;
	const auto copied =
		is_descending(side)
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
	// Against window.begin(): the result iterator is the span's, which is not a
	// raw pointer under a hardened standard library.
	const auto written = static_cast<std::size_t>(copied.out - window.begin());

	// A duplicated price would break the binary search every other operation
	// relies on; a well-formed snapshot has none, and the first wins if one
	// ever does. Duplicates are adjacent now that the window is sorted.
	const auto surplus =
		std::ranges::unique(window.first(written), {}, &price_level::price);
	s.size = written - static_cast<std::size_t>(std::ranges::distance(surplus));

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
	const std::span<const price_level> levels = bid_levels();
	if (levels.empty()) return std::nullopt;
	return levels.front().price;
}

std::optional<scaled_price_t> l2_book::best_ask() const noexcept {
	const std::span<const price_level> levels = ask_levels();
	if (levels.empty()) return std::nullopt;
	return levels.front().price;
}

bool l2_book::is_crossed() const noexcept {
	const std::span<const price_level> bid = bid_levels();
	const std::span<const price_level> ask = ask_levels();
	// One side empty is not a cross - it is a book with nothing to cross with.
	if (bid.empty() || ask.empty()) return false;
	return bid.front().price >= ask.front().price;
}

scaled_qty_t l2_book::volume_at_price(scaled_price_t price, side_t side) const {
	const std::span<const price_level> levels = side_levels(side);

	const std::size_t at = seek(levels, price, side);
	return hit(levels, at, price) ? levels[at].qty : 0;
}

std::size_t l2_book::depth(side_t side) const noexcept {
	return side_levels(side).size();
}

std::size_t l2_book::max_depth() const noexcept { return max_depth_; }

std::uint64_t l2_book::dropped_levels() const noexcept {
	return dropped_levels_;
}

std::span<const price_level> l2_book::bid_levels() const noexcept {
	return cells().first(bid_size_);
}

std::span<const price_level> l2_book::ask_levels() const noexcept {
	return cells().subspan(max_depth_, ask_size_);
}

depth_sweep l2_book::sweep(std::span<const price_level> levels, side_t side,
						   scaled_qty_t size) noexcept {
	depth_sweep result{.side      = side,
					   .requested = size,
					   .filled    = 0,
					   .touch     = 0,
					   .last      = 0,
					   .levels    = 0};
	if (size <= 0 || levels.empty()) return result;

	result.touch = levels.front().price;
	result.last  = levels.front().price;

	// A register of levels at a time, and level-by-level only inside the one
	// block the size runs out in. @see core::simd::consume.
	const auto taken = core::simd::consume_interleaved(as_pairs(levels), size);
	result.filled    = taken.filled;
	result.levels    = taken.levels;
	if (taken.levels != 0) result.last = levels[taken.levels - 1].price;

	// Every retained cell carries real depth: set_level erases a level at a
	// non-positive size rather than storing one, and load filters those out
	// before they can occupy a slot. So there is no empty cell to skip, and no
	// level counted that contributed nothing to the fill.
	//
	// The check used to sit inside the walk, one level at a time. It is stated
	// after the fact now because the walk no longer visits levels one at a
	// time - the vector loop sums a register without looking at a lane - and
	// re-deriving it per lane would cost the kernel what it was written to
	// save. The scope is the same levels the old assertion covered, and it
	// compiles out with NDEBUG exactly as that one did.
	assert(std::ranges::all_of(levels.first(result.levels),
							   [](const price_level &level) {
								   return level.qty > 0;
							   }) &&
		   "a non-positive size is not a level");
	return result;
}

depth_sweep l2_book::sweep_asks(scaled_qty_t size) const noexcept {
	return sweep(ask_levels(), side_t::ask, size);
}

depth_sweep l2_book::sweep_bids(scaled_qty_t size) const noexcept {
	return sweep(bid_levels(), side_t::bid, size);
}

scaled_qty_t l2_book::total_volume(side_t side) const noexcept {
	return core::simd::total_interleaved(as_pairs(side_levels(side)));
}

std::size_t l2_book::size() const noexcept { return bid_size_ + ask_size_; }

bool l2_book::is_empty() const noexcept { return size() == 0; }

std::span<price_level> l2_book::cells() noexcept {
	return {cells_.get(), max_depth_ * 2};
}

std::span<const price_level> l2_book::cells() const noexcept {
	return {cells_.get(), max_depth_ * 2};
}

l2_book::side_view l2_book::mutable_side(side_t side) noexcept {
	// Which half of the block, not which way it sorts - see is_descending.
	const bool is_bid = side == side_t::bid;
	return {.block = cells().subspan(is_bid ? 0 : max_depth_, max_depth_),
			.size  = is_bid ? bid_size_ : ask_size_};
}

std::span<const price_level> l2_book::side_levels(side_t side) const noexcept {
	return side == side_t::bid ? bid_levels() : ask_levels();
}

std::span<price_level> l2_book::side_view::live() const noexcept {
	return block.first(size);
}
} // namespace exchange::market_data
