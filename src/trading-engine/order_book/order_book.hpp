#pragma once
#include "detail/book_side.hpp"
#include "fwd.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace exchange::engine {

/**
 * @brief Price-time-priority matching engine.
 *
 * Each price level holds a FIFO of individual resting orders (oldest first) as
 * an intrusive list of nodes drawn from one pool owned by this book. The two
 * sides are book_side objects wrapping sorted vectors of levels: bids
 * descending, asks ascending, so the best price is always front().
 *
 * Nothing on the matching path allocates once the pool has warmed: resting an
 * order takes a pool slot, a fill unlinks one, and both sides' levels are flat
 * scalar cells that shift by memmove. Cancel is a hash lookup for the order's
 * location plus an O(1) unlink, since the location carries the node itself.
 *
 * @par Entry points
 * - place_order:  matching entry point (crosses, then rests the remainder)
 * - cancel_order: cancel a resting order by id via the id->location index
 * - add_order:    rest anonymous liquidity, no matching (seed/benchmark helper)
 * - delete_order: reduce resting qty at a price, FIFO-first
 */
class order_book {
public:
	/**
	 * @brief Construct an order book.
	 * @param capacity Expected number of simultaneously resting orders. The
	 *        node pool is reserved to it up front, so a book that stays within
	 *        the hint never grows its storage while matching; exceeding it is
	 *        correct but pays one reallocation.
	 */
	TRADING_ENGINE_EXPORT explicit order_book(std::size_t capacity = 1u << 15);

	/**
	 * @brief Matching entry point: cross @p incoming against the opposite side,
	 *        then rest the unfilled remainder per its OrderType.
	 *
	 * Fills are appended to @p out (never cleared) so the matching engine can
	 * accumulate a whole drain's trades into one reused buffer.
	 * GOOD_TILL_CANCELLED rests any remainder; IMMEDIATE_OR_CANCEL drops it;
	 * FILL_OR_KILL executes only if the whole quantity can be filled now,
	 * otherwise it is a no-op.
	 */
	TRADING_ENGINE_EXPORT void place_order(const Order &incoming,
										   std::vector<Trade> &out);

	/// @brief Convenience overload: match @p incoming and return its fills.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::vector<Trade>
	place_order(const Order &incoming);

	/**
	 * @brief Rest anonymous liquidity at a price without matching.
	 *
	 * Seed/benchmark helper: the order carries no identity (not tracked for
	 * cancel-by-id) and no crossing check is performed.
	 */
	TRADING_ENGINE_EXPORT void add_order(side side, price price, quantity volume);

	/**
	 * @brief Cancel a previously placed (identified) order.
	 * @param id Identifier of the order to cancel.
	 * @note No-op if @p id is unknown or already fully filled.
	 */
	TRADING_ENGINE_EXPORT void cancel_order(order_id id);


	/**
	 * @brief Reduce resting qty at a price, draining whole orders
	 * FIFO-first.
	 * @param side Book side.
	 * @param price Price level to reduce.
	 * @param qty Quantity to remove.
	 */
	TRADING_ENGINE_EXPORT void delete_order(side side, price price,
											quantity volume);

	/**
	 * @brief Set the aggregate resting qty at a price to an absolute value.
	 *
	 * This is the L2 diff-feed primitive: a Binance @c depthUpdate carries the
	 * new
	 * @em absolute quantity for each touched level, not a delta. Applying one
	 * is "set this price to this size", where a size of 0 removes the level.
	 * The level is collapsed to a single anonymous aggregate — individual-order
	 * identity and FIFO priority are not modelled at L2, so this must not be
	 * mixed with place_order()/cancel_order() flow on the same book.
	 *
	 * @param side Book side to update.
	 * @param price Price level to set.
	 * @param qty New absolute aggregate qty; <= 0 removes the level.
	 * @note O(1) when the level already exists (the common replay case).
	 */
	TRADING_ENGINE_EXPORT void set_level(side side, price price, quantity volume);

	/**
	 * @brief Aggregate resting qty at a price on a side.
	 * @param price Price level to query.
	 * @param side Book side.
	 * @return The total resting qty, or 0 if the level does not exist.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity volume_at_price(price price,
															   side side) const;

	/// @brief Best (highest) bid_ price, or std::nullopt if no bids rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<price> best_bid() const;

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<price> best_ask() const;

private:
	/// @brief Where a live order sits, for cancel by id.
	///
	/// The node index is what makes cancel O(1): side and price find the level
	/// in O(log n), and the node then splices straight out of that level's FIFO
	/// with no scan for the matching id.
	struct Location {
		side side;
		price price;
		detail::node_index node;
	};

	static constexpr order_id kAnonymous =
		0; ///< reserved: not tracked in index_

	/**
	 * @brief Would @p incoming trade against a level resting at @p book_price?
	 */
	static bool is_price_crossing(const Order &incoming, price book_price);

	/// @brief Would a @p side order at @p price trade against @p book_price?
	static bool is_price_crossing(side side, price p, price book_price);

	detail::book_side &side_levels(side s);
	[[nodiscard]] const detail::book_side &side_levels(side s) const;

	/// @brief Drop the fully-filled front order of @p level, clearing its id
	///        index entry.
	void pop_front(Level &level);

	/// @brief True if @p qty can be fully filled against @p opposite now.
	[[nodiscard]] bool can_fully_fill(const detail::book_side &opposite,
									  side side, price price,
									  quantity volume) const;

	/// Declared before the sides: both bind a reference to it at construction,
	/// and members initialise in declaration order.
	detail::order_pool pool_;
	detail::book_side bid_; ///< descending by price (best = front)
	detail::book_side ask_; ///< ascending by price (best = front)
	std::unordered_map<order_id, Location> index_;
};

} // namespace exchange::engine
