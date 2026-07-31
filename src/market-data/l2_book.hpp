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

	/// @brief A @c max_depth meaning "retain every level the venue publishes".
	static constexpr std::size_t UNBOUNDED_DEPTH = 0;

	/**
	 * @brief Construct a book retaining at most @p max_depth levels per side.
	 *
	 * The cap exists for one reason: @c set_level's insert and erase paths
	 * memmove the tail of a side, so their cost is linear in retained depth
	 * while the overwrite path is flat. A book that keeps 1000 levels pays that
	 * shift on every new price near the touch — which is where a diff feed puts
	 * almost all of them — and a consumer that only ever reads the top 10-50
	 * levels pays it for depth it never looks at. Capping the side is what turns
	 * an unbounded shift into a bounded one, and it is the only lever that
	 * brings the insert path under a per-update latency budget without changing
	 * the layout.
	 *
	 * Capping is not free of meaning: a capped book is a @b top-N view, not an
	 * exact replica. An L2 diff feed only reports prices whose size changed, so
	 * once a level falls outside the window its size is forgotten and cannot be
	 * recovered from the stream — the venue will not resend it until it changes
	 * again. Beyond the window, @c volume_at_price therefore returns 0 for
	 * "outside the retained view" exactly as it does for "no level here", and
	 * the two are indistinguishable. Choose a cap comfortably above the deepest
	 * level any consumer reads, and use @c UNBOUNDED_DEPTH when a consumer
	 * genuinely needs full published depth.
	 *
	 * @param max_depth Levels retained per side, or @c UNBOUNDED_DEPTH for all.
	 * @note A capped book reserves @p max_depth cells per side up front. That is
	 *       deliberate: with the capacity already in place an insert is a
	 *       memmove and never a reallocation, which is what keeps the allocator
	 *       — and its unbounded tail — off the update path entirely.
	 */
	MARKET_DATA_EXPORT explicit l2_book(std::size_t max_depth = UNBOUNDED_DEPTH);

	/**
	 * @brief Set the absolute aggregate size at @p price on @p side.
	 *
	 * The L2 diff primitive: a @c qty <= 0 removes the level; otherwise the
	 * level is created (in sorted position) or its size overwritten. O(1) to
	 * update an existing level; O(log n) search plus O(n) shift to insert or
	 * erase.
	 *
	 * That shift is the expensive path and clustering does @b not make it cheap.
	 * A side is stored best-first, so a new price near the touch shifts nearly
	 * every level behind it while the worst price shifts none — the top-of-book
	 * concentration a diff feed exhibits lands its inserts on the maximum-shift
	 * end, not the cheap one. Measured (order_latency, 1000 levels/side, p99):
	 * ~38 ns to overwrite, 300-390 ns to insert near the touch. Depth is what the
	 * shift is linear in, which is what @c max_depth exists to bound.
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

	/// @brief The per-side retention cap, or @c UNBOUNDED_DEPTH when uncapped.
	[[nodiscard]] MARKET_DATA_EXPORT std::size_t max_depth() const noexcept;

	/**
	 * @brief The bid side, best (highest) price first.
	 *
	 * Reads name their side rather than taking a @c side_t, matching
	 * @c best_bid / @c best_ask. Nothing crosses sides on a reconstruction book
	 * — it does not match, so it never needs @c opposed() — and every read call
	 * site in the tree knows its side at compile time, so a parametric reader
	 * would only add a branch to undo one the caller had already resolved.
	 *
	 * The mutating half stays parametric: @c set_level and @c load have callers
	 * carrying a genuinely runtime side (the streaming decoder walking the bid
	 * then ask array, and the matching engine applying a command off the wire).
	 *
	 * @note Inline on purpose. Every exported member is an out-of-line
	 *       cross-module call; these two are a member read.
	 */
	[[nodiscard]] const std::vector<Level> &bid_levels() const noexcept {
		return bids_;
	}

	/// @brief The ask side, best (lowest) price first. @see bid_levels
	[[nodiscard]] const std::vector<Level> &ask_levels() const noexcept {
		return asks_;
	}

private:
	// bids_: descending by price (best = highest = front)
	// asks_: ascending  by price (best = lowest  = front)
	std::vector<Level> bids_;
	std::vector<Level> asks_;
	std::size_t max_depth_ = UNBOUNDED_DEPTH;
};

} // namespace exchange::market_data
