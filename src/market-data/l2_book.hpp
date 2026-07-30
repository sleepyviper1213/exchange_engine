#pragma once
#include "core/types.hpp"
#include "fwd.hpp"
#include "market_data_export.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace exchange::market_data {

/**
 * @brief A cache-optimised Level-2 (aggregate-by-price) book for the managed
 *        local-order-book reconstruction path.
 *
 * Each side is a single contiguous, price-sorted array of {price, qty} cells
 * — bids descending, asks ascending, so the best price is always @c front().
 * This is market data's own view of the depth an exchange @em publishes: the L2
 * diff feed only ever carries an absolute aggregate size per price, so a flat
 * array is all that is needed and all that should be paid for. @c set_level is
 * a binary search plus an in-place qty write (or a shift on insert/erase),
 * best bid/ask is @c front(), and a top-of-book walk is sequential over
 * contiguous memory with no per-level pointer chase. At 16 bytes per cell, four
 * levels share a cache line.
 *
 * This is a reconstruction / quote book: it models absolute L2 sizes (a size of
 * 0 removes the price) and deliberately does @b not match, track order
 * identity, or model FIFO priority. Those belong to @c engine::order_book, the
 * trading engine's order-by-order (L3) book that keeps a FIFO of individual @c
 * Order objects per level — a different concept in a different subsystem. Do
 * not mix this with @c order_book's place_order()/cancel_order() flow.
 */
class l2_book {
public:
	/// @brief One aggregated price level: a price and the total size resting on
	///        it. Trivially copyable and 16 bytes so a side packs densely.
	struct Level {
		price_t price;
		quantity_t qty;
	};

	/**
	 * @brief Set the absolute aggregate size at @p price on @p side.
	 *
	 * The L2 diff primitive: a @c qty <= 0 removes the level; otherwise the
	 * level is created (in sorted position) or its size overwritten. O(1) to
	 * update an existing level; O(log n) search plus O(n) shift to insert or
	 * erase — cheap in practice because feed updates cluster near top of book.
	 */
	MARKET_DATA_EXPORT void set_level(side_t side, price_t price, quantity_t volume);

	/**
	 * @brief Replace @p side's levels wholesale with @p levels — the snapshot
	 *        seed path.
	 *
	 * Takes ownership, then puts the side straight into its invariant: levels
	 * with a non-positive size dropped (an absent price and a zero-size price
	 * are the same state), sorted best-first, and at most one level per price.
	 * The caller therefore need not know how a venue orders a snapshot, which is
	 * the point — feeding the same levels through @c set_level one at a time
	 * costs O(n) per insert in whatever order the venue happens not to use.
	 * @param side The side to replace.
	 * @param levels The side's complete depth, in any order.
	 */
	MARKET_DATA_EXPORT void load(side_t side, std::vector<Level> levels);

	/// @brief Drop every level on both sides, keeping the arrays' capacity.
	MARKET_DATA_EXPORT void clear() noexcept;

	/// @brief Best (highest) bid price, or std::nullopt if no bids rest.
	[[nodiscard]] MARKET_DATA_EXPORT std::optional<price_t>
	best_bid() const noexcept;

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] MARKET_DATA_EXPORT std::optional<price_t>
	best_ask() const noexcept;

	/// @brief Aggregate size at @p price on @p side, or 0 if no level rests
	///        there.
	[[nodiscard]] MARKET_DATA_EXPORT quantity_t volume_at_price(price_t price,
															side_t side) const;

	/// @brief Number of resting levels on @p side.
	[[nodiscard]] MARKET_DATA_EXPORT std::size_t
	depth(side_t side) const noexcept;

	/// @brief Read-only, best-first view of a side's contiguous levels.
	[[nodiscard]] MARKET_DATA_EXPORT const std::vector<Level> &
	levels(side_t side) const noexcept;

private:
	// bids_: descending by price (best = highest = front)
	// asks_: ascending  by price (best = lowest  = front)
	std::vector<Level> bids_;
	std::vector<Level> asks_;
};

} // namespace exchange::market_data
