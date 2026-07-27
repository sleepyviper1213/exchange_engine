#pragma once
#include "fwd.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace exchange::engine {

/**
 * @brief A cache-optimized Level-2 (aggregate-by-price) book for the managed
 *        local-order-book reconstruction path.
 *
 * Each side is a single contiguous, price-sorted array of {price, volume} cells
 * — bids descending, asks ascending, so the best price is always @c front().
 * Unlike @c order_book, which keeps a heap-allocated FIFO of individual @c Order
 * objects per level for L3 matching, the L2 diff feed only ever carries an
 * absolute aggregate size per price. So a flat array is all that is needed and
 * all that should be paid for: @c set_level is a binary search plus an in-place
 * volume write (or a shift on insert/erase), best bid/ask is @c front(), and a
 * top-of-book walk is sequential over contiguous memory with no per-level
 * pointer chase. At 16 bytes per cell, four levels share a cache line.
 *
 * This is a reconstruction / quote book: it models absolute L2 sizes (a size of
 * 0 removes the price) and deliberately does @b not match, track order identity,
 * or model FIFO priority. Do not mix it with @c order_book's
 * place_order()/cancel_order() flow.
 */
class l2_book {
public:
	/// @brief One aggregated price level: a price and the total size resting on
	///        it. Trivially copyable and 16 bytes so a side packs densely.
	struct Level {
		Price price;
		Volume volume;
	};

	/**
	 * @brief Set the absolute aggregate size at @p price on @p side.
	 *
	 * The L2 diff primitive: a @c volume <= 0 removes the level; otherwise the
	 * level is created (in sorted position) or its size overwritten. O(1) to
	 * update an existing level; O(log n) search plus O(n) shift to insert or
	 * erase — cheap in practice because feed updates cluster near top of book.
	 */
	TRADING_ENGINE_EXPORT void set_level(Side side, Price price, Volume volume);

	/// @brief Drop every level on both sides, keeping the arrays' capacity.
	TRADING_ENGINE_EXPORT void clear() noexcept;

	/// @brief Best (highest) bid price, or std::nullopt if no bids rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<Price>
	best_bid() const noexcept;

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<Price>
	best_ask() const noexcept;

	/// @brief Aggregate size at @p price on @p side, or 0 if no level rests
	///        there.
	[[nodiscard]] TRADING_ENGINE_EXPORT Volume
	volume_at_price(Price price, Side side) const;

	/// @brief Number of resting levels on @p side.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t
	depth(Side side) const noexcept;

	/// @brief Read-only, best-first view of a side's contiguous levels.
	[[nodiscard]] TRADING_ENGINE_EXPORT const std::vector<Level> &
	levels(Side side) const noexcept;

private:
	// bids_: descending by price (best = highest = front)
	// asks_: ascending  by price (best = lowest  = front)
	std::vector<Level> bids_;
	std::vector<Level> asks_;
};

} // namespace exchange::engine
