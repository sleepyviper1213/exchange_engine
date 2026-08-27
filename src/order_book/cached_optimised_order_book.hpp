#pragma once
#include "core/optimisation/branchless_binary_search.hpp"
#include "core/util/attributes.hpp"
#include "orders/cached_optimised_level.hpp"
#include "orders/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <span>

#ifdef _MSC_VER
#include <intrin.h>
#endif


#ifdef _MSC_VER
#define EXCHANGE_PREFETCH(addr)                                                \
	_mm_prefetch(                                                              \
		reinterpret_cast<const char *>(addr),                                  \
		_MM_HINT_T0) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

#else
#define EXCHANGE_PREFETCH(addr)                                                \
	__builtin_prefetch(static_cast<const void *>(addr), 0, 3)
#endif

namespace exchange::engine::experimental {

/**
 * @brief Fixed-capacity aggregate-by-price book that caches its own touch.
 *
 * Each side is a @c std::array of @c cache_optimised_level held price-sorted
 * best-first - bids descending, asks ascending - so the best price is always
 * @c [0] and a depth walk is sequential over contiguous storage. The capacity
 * is a template parameter rather than a constructor argument, so the levels are
 * in-class storage: the book never reaches an allocator, and one embedded in a
 * per-symbol array lands inline with its neighbours instead of behind a
 * pointer.
 *
 * The "cached" half of the name is @c best_bid_ / @c best_ask_ / @c spread_.
 * Every mutation already knows whether it disturbed index 0, so the touch is
 * maintained as it goes and the readers become a load rather than an emptiness
 * check plus an indexed read through the level array. That is the trade this
 * class exists to make against @c market_data::l2_book, which recomputes from
 * @c front().
 *
 * @par Relationship to the rest of the tree
 * This is an aggregate (L2) book: it models absolute sizes per price, has no
 * order identity and no FIFO priority, and does @b not match. The trading
 * engine's order-by-order book is @c engine::order_book; market data's
 * reconstruction book is @c market_data::l2_book, whose semantics this mirrors
 * deliberately so the two are not surprising side by side.
 *
 * @tparam N Levels retained per side. Both sides get @c N, so the book holds at
 *         most @c 2*N levels.
 */
template <std::size_t N>
class cached_optimised_order_book {
public:
	static_assert(N > 0, "a side needs room for at least one level");
	static_assert(N <= std::numeric_limits<std::uint16_t>::max(),
				  "per-side depth is counted in a std::uint16_t");

	using level_type = cache_optimised_level;

	/// @brief Levels retained per side, fixed at compile time.
	[[nodiscard]] static constexpr std::size_t max_depth() noexcept {
		return N;
	}

	/**
	 * @brief Set the absolute aggregate size at @p price on @p side.
	 *
	 * The L2 diff primitive, and the only mutator. A @c quantity <= 0 removes
	 * the level (an absent price and a zero-size price are the same state);
	 * otherwise the level is created in sorted position or its size
	 * overwritten. Overwriting is a search plus a store; insert and erase
	 * additionally shift the tail of the side, which is what bounding the side
	 * at @c N bounds.
	 *
	 * When a side is full, the level that does not fit is the @em worst one: a
	 * new price better than the resting worst evicts it, and a new price worse
	 * than all @c N is refused. Either way the book stays a true top-@c N view
	 * - the touch is never what gets dropped - and the loss is counted in
	 * @c dropped_levels().
	 */
	void update_level(side_t side, price_t price, quantity_t quantity) {
		++timestamp_;

		// Bids sort descending and asks ascending, so the comparator differs by
		// side. Dispatching once here keeps it a compile-time constant inside
		// the search, which is the point of the branchless probe loop.
		if (side == side_t::bid) {
			apply<std::ranges::greater>(bid_levels_,
										bid_count_,
										price,
										quantity);
		} else {
			apply<std::ranges::less>(ask_levels_, ask_count_, price, quantity);
		}
		refresh_touch();
	}

	/// @brief Drop every level on both sides. The storage stays where it is.
	void clear() noexcept {
		bid_count_ = 0;
		ask_count_ = 0;
		refresh_touch();
	}

	/// @brief Best (highest) bid price, or std::nullopt if no bids rest.
	[[nodiscard]] std::optional<price_t> best_bid() const noexcept {
		if (bid_count_ == 0) return std::nullopt;
		return best_bid_;
	}

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] std::optional<price_t> best_ask() const noexcept {
		if (ask_count_ == 0) return std::nullopt;
		return best_ask_;
	}

	/**
	 * @brief Best ask minus best bid.
	 *
	 * @return std::nullopt when either side is empty, and when the book is
	 *         crossed - @c price_t is unsigned, so a crossed book has no
	 *         representable spread and returning one would wrap. A caller that
	 *         wants to know which case it hit should ask @c is_crossed().
	 */
	[[nodiscard]] std::optional<price_t> spread() const noexcept {
		if (bid_count_ == 0 || ask_count_ == 0 || is_crossed())
			return std::nullopt;
		return spread_;
	}

	/// @brief Is the best bid at or above the best ask?
	/// @note Locked (bid == ask) counts as crossed, matching
	///       @c market_data::l2_book.
	[[nodiscard]] bool is_crossed() const noexcept {
		return bid_count_ != 0 && ask_count_ != 0 && best_bid_ >= best_ask_;
	}

	/// @brief Aggregate size at @p price on @p side, or 0 if no level rests
	///        there. A price outside the retained window is indistinguishable
	///        from an absent one, and both read 0.
	[[nodiscard]] quantity_t volume_at_price(price_t price,
											 side_t side) const noexcept {
		const auto *const level = find(price, side);
		return level != nullptr ? level->volume : 0;
	}

	/// @brief The level resting at @p price on @p side, running statistics and
	///        all, or std::nullopt if none does.
	[[nodiscard]] std::optional<level_type>
	level_at_price(price_t price, side_t side) const noexcept {
		const auto *const level = find(price, side);
		if (level == nullptr) return std::nullopt;
		return *level;
	}

	/// @brief Number of resting levels on @p side.
	[[nodiscard]] std::size_t depth(side_t side) const noexcept {
		return side == side_t::bid ? bid_count_ : ask_count_;
	}

	/// @brief Resting levels across both sides. Levels, not orders or volume.
	[[nodiscard]] std::size_t size() const noexcept {
		return std::size_t{bid_count_} + std::size_t{ask_count_};
	}

	/// @brief True when no level rests on either side.
	[[nodiscard]] bool is_empty() const noexcept { return size() == 0; }

	/// @brief The bid side, best (highest) price first.
	/// @note The span covers only the resting levels; storage beyond
	///       @c depth(bid) is not part of the book's state.
	[[nodiscard]] std::span<const level_type>
	bid_levels() const noexcept EXCHANGE_LIFETIMEBOUND {
		return {bid_levels_.data(), bid_count_};
	}

	/// @brief The ask side, best (lowest) price first. @see bid_levels
	[[nodiscard]] std::span<const level_type>
	ask_levels() const noexcept EXCHANGE_LIFETIMEBOUND {
		return {ask_levels_.data(), ask_count_};
	}

	/// @brief The side named by @p side, best price first.
	[[nodiscard]] std::span<const level_type>
	levels_for(side_t side) const noexcept EXCHANGE_LIFETIMEBOUND {
		return side == side_t::bid ? bid_levels() : ask_levels();
	}

	/**
	 * @brief Levels the window refused or evicted since construction.
	 *
	 * The cost of the fixed capacity, made countable: one per @c update_level
	 * that landed outside a full side or pushed the worst level out of it. A
	 * book sized right for its feed reports a small and stable number; one
	 * climbing steadily is discarding depth its readers see as zero.
	 */
	[[nodiscard]] std::uint64_t dropped_levels() const noexcept {
		return dropped_levels_;
	}

	/// @brief Calls to @c update_level since construction, whether or not they
	///        changed anything. This is the clock @c level_type::timestamp is
	///        stamped from, so a level's stamp says how long ago the feed last
	///        wrote a size at that price.
	/// @note The clock is 64-bit and this reports it whole; the per-level stamp
	///       is the low 32 bits, so comparing stamps across more than 2^32
	///       updates is only meaningful through this value.
	[[nodiscard]] std::uint64_t update_count() const noexcept {
		return timestamp_;
	}

private:
	/// @brief The level resting at @p price on @p side, or nullptr.
	[[nodiscard]] const level_type *find(price_t price,
										 side_t side) const noexcept {
		const auto levels = levels_for(side);
		const auto index  = side == side_t::bid
								? locate<std::ranges::greater>(levels, price)
								: locate<std::ranges::less>(levels, price);
		if (index == levels.size() || levels[index].price != price)
			return nullptr;
		return &levels[index];
	}

	/// @brief Index of the first level not ordered before @p price under
	///        @c Compare - the position @p price belongs at, best-first.
	template <class Compare>
	[[nodiscard]] static std::size_t locate(std::span<const level_type> levels,
											price_t price) noexcept {
		const auto iter =
			core::optimisation::branchless_lower_bound(levels,
													   price,
													   Compare{},
													   &level_type::price);
		return static_cast<std::size_t>(iter - levels.begin());
	}

	/// @brief Insert, overwrite or erase @p price on one side, keeping it
	///        sorted best-first under @c Compare.
	template <class Compare>
	void apply(std::array<level_type, N> &levels, std::uint16_t &count,
			   price_t price, quantity_t quantity) {
		const std::size_t live  = count;
		const std::size_t index = locate<Compare>({levels.data(), live}, price);
		const bool found        = index < live && levels[index].price == price;

		if (found) {
			if (quantity <= 0) {
				erase_at(levels, count, index);
			} else {
				levels[index].volume = quantity;
				record(levels[index], quantity, timestamp_);
			}
			return;
		}

		// Removing a level that is not there is not an error: a feed may report
		// a size of 0 for a price this window never retained.
		if (quantity <= 0) return;

		if (live == N) {
			// The side is full, and `index` is where the new price sorts, so
			// index == N means every resting level is better than this one.
			++dropped_levels_;
			if (index == N) return;
			--count; // evict the worst level to make room
		}

		insert_at(levels, count, index, price, quantity, timestamp_);
	}

	/// @brief Open a slot at @p index by shifting the tail right, then seed it.
	static void insert_at(std::array<level_type, N> &levels,
						  std::uint16_t &count, std::size_t index,
						  price_t price, quantity_t quantity,
						  std::uint64_t stamp) {
		const auto first =
			std::next(levels.begin(), static_cast<std::ptrdiff_t>(index));
		const auto last =
			std::next(levels.begin(), static_cast<std::ptrdiff_t>(count));

		// The search probed log2(count) scattered lines; the shift is about to
		// stream the whole tail. Pull the far end in while the moves issue.
		if (first != last) EXCHANGE_PREFETCH(&*std::prev(last));

		std::move_backward(first, last, std::next(last));
		++count;

		levels[index] = level_type{.price          = price,
								   .volume         = quantity,
								   .count          = 0,
								   .timestamp      = 0,
								   .total_volume   = 0,
								   .avg_order_size = 0};
		record(levels[index], quantity, stamp);
	}

	/// @brief Close the slot at @p index by shifting the tail left.
	static void erase_at(std::array<level_type, N> &levels,
						 std::uint16_t &count, std::size_t index) {
		const auto slot =
			std::next(levels.begin(), static_cast<std::ptrdiff_t>(index));
		const auto last =
			std::next(levels.begin(), static_cast<std::ptrdiff_t>(count));
		std::move(std::next(slot), last, slot);
		--count;
	}

	/**
	 * @brief Fold one write into a level's running statistics.
	 *
	 * @c count is the writes landed on this level since it was created and
	 * @c total_volume their sum, so @c avg_order_size is the mean size the feed
	 * has reported at this price. An aggregate book sees sizes, never orders:
	 * these describe the @em update stream, not a population of resting orders,
	 * and a level that leaves the window and comes back starts again from zero.
	 */
	static void record(level_type &level, quantity_t quantity,
					   std::uint64_t stamp) {
		++level.count;
		level.timestamp = static_cast<uint32_t>(stamp & 0xFFFF'FFFF);
		level.total_volume += static_cast<std::uint64_t>(quantity);

		const std::uint64_t mean = level.total_volume / level.count;
		level.avg_order_size     = static_cast<std::uint32_t>(
			std::min<std::uint64_t>(mean,
									std::numeric_limits<std::uint32_t>::max()));
	}

	/**
	 * @brief Recompute the cached touch from index 0 of each side.
	 *
	 * Called once per mutation rather than folded into the shift paths: an
	 * insert or erase at index 0 moves the touch, an insert further down can
	 * still evict the worst level, and a refusal by a full side moves nothing.
	 * One unconditional pair of loads off lines the update just touched is
	 * cheaper than getting that case analysis wrong.
	 */
	void refresh_touch() noexcept {
		best_bid_ = bid_count_ != 0 ? bid_levels_[0].price : price_t{0};
		best_ask_ = ask_count_ != 0 ? ask_levels_[0].price : price_t{0};
		spread_ = (bid_count_ != 0 && ask_count_ != 0 && best_ask_ > best_bid_)
					  ? best_ask_ - best_bid_
					  : price_t{0};
	}

	std::array<level_type, N> bid_levels_{}; ///< descending; best = [0]
	std::array<level_type, N> ask_levels_{}; ///< ascending;  best = [0]

	std::uint16_t bid_count_ = 0;
	std::uint16_t ask_count_ = 0;

	/// Meaningful only while the matching count is non-zero. The accessors gate
	/// on that rather than reserving a sentinel price, since every @c price_t
	/// is a price some venue could legitimately quote.
	price_t best_bid_ = 0;
	price_t best_ask_ = 0;
	price_t spread_   = 0;

	std::uint64_t timestamp_      = 0;
	std::uint64_t dropped_levels_ = 0;
};

} // namespace exchange::engine::experimental

#undef EXCHANGE_PREFETCH
