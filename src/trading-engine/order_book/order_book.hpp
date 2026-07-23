#pragma once
#include "book_side.hpp"
#include "level.hpp"
#include "order.hpp"
#include "trade.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace order_book {

/**
 * @brief Price-time-priority matching engine.
 *
 * Each price level holds a FIFO of individual resting orders in a plain vector
 * (oldest first). The two sides are book_side objects wrapping sorted vectors of
 * levels: bids descending, asks ascending, so the best price is always front().
 *
 * @note The intrusive pool-backed order list is temporarily set aside; orders
 *       are stored by value in each level's vector.
 *
 * @par Entry points
 * - place_order:  matching entry point (crosses, then rests the remainder)
 * - cancel_order: cancel a resting order by id via the id->location index
 * - add_order:    rest anonymous liquidity, no matching (seed/benchmark helper)
 * - delete_order: reduce resting volume at a price, FIFO-first
 */
class OrderBook {
public:
    /**
     * @brief Construct an order book.
     * @param capacity Hint for the maximum number of simultaneously resting
     *        orders (currently advisory only).
     */
    ORDER_BOOK_EXPORT explicit OrderBook(std::size_t capacity = 1u << 15);

    /**
     * @brief Matching entry point: cross @p incoming against the opposite side,
     *        then rest the unfilled remainder per its OrderType.
     *
     * Fills are appended to @p out (never cleared) so the matching engine can
     * accumulate a whole drain's trades into one reused buffer. GOOD_TILL_CANCELED
     * rests any remainder; IMMEDIATE_OR_CANCEL drops it; FILL_OR_KILL executes
     * only if the whole quantity can be filled now, otherwise it is a no-op.
     */
    ORDER_BOOK_EXPORT void place_order(const Order &incoming,
                                       std::vector<Trade> &out);

    /// @brief Convenience overload: match @p incoming and return its fills.
    [[nodiscard]] ORDER_BOOK_EXPORT std::vector<Trade>
    place_order(const Order &incoming);

    /**
     * @brief Rest anonymous liquidity at a price without matching.
     *
     * Seed/benchmark helper: the order carries no identity (not tracked for
     * cancel-by-id) and no crossing check is performed.
     */
    ORDER_BOOK_EXPORT void add_order(Side side, Price price, Volume volume);

    /**
     * @brief Cancel a previously placed (identified) order.
     * @param id Identifier of the order to cancel.
     * @note No-op if @p id is unknown or already fully filled.
     */
    ORDER_BOOK_EXPORT void cancel_order(OrderId id);


    /**
     * @brief Reduce resting volume at a price, draining whole orders FIFO-first.
     * @param side Book side.
     * @param price Price level to reduce.
     * @param volume Quantity to remove.
     */
    ORDER_BOOK_EXPORT void delete_order(Side side, Price price, Volume volume);

    /**
     * @brief Set the aggregate resting volume at a price to an absolute value.
     *
     * This is the L2 diff-feed primitive: a Binance @c depthUpdate carries the new
     * @em absolute quantity for each touched level, not a delta. Applying one is
     * "set this price to this size", where a size of 0 removes the level. The level
     * is collapsed to a single anonymous aggregate — individual-order identity and
     * FIFO priority are not modelled at L2, so this must not be mixed with
     * place_order()/cancel_order() flow on the same book.
     *
     * @param side Book side to update.
     * @param price Price level to set.
     * @param volume New absolute aggregate volume; <= 0 removes the level.
     * @note O(1) when the level already exists (the common replay case).
     */
    ORDER_BOOK_EXPORT void set_level(Side side, Price price, Volume volume);

    /**
     * @brief Aggregate resting volume at a price on a side.
     * @param price Price level to query.
     * @param side Book side.
     * @return The total resting volume, or 0 if the level does not exist.
     */
    [[nodiscard]] ORDER_BOOK_EXPORT Volume volume_at_price(Price price,
                                                           Side side) const;

    /// @brief Best (highest) bid_ price, or std::nullopt if no bids rest.
    [[nodiscard]] ORDER_BOOK_EXPORT std::optional<Price> best_bid() const;

    /// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
    [[nodiscard]] ORDER_BOOK_EXPORT std::optional<Price> best_ask() const;

private:
    /// @brief Where a live order sits, for cancel by id.
    struct Location {
        Side side;
        Price price;
    };

    static constexpr OrderId kAnonymous = 0; ///< reserved: not tracked in index_

    /**
     * @brief Would @p incoming trade against a level resting at @p book_price?
     */
    static bool is_price_crossing(const Order &incoming, Price book_price);

    /// @brief Would a @p side order at @p price trade against @p book_price?
    static bool is_price_crossing(Side side, Price price, Price book_price);

	book_side& side_levels(Side s);
	const book_side& side_levels(Side s) const;

    /// @brief Drop the fully-filled front order of @p level, clearing its id
    ///        index entry.
    void pop_front(Level &level);

    /// @brief True if @p volume can be fully filled against @p opposite now.
    bool can_fully_fill(const book_side &opposite, Side side,
                        Price price, Volume volume) const;

	book_side bid_; ///< descending by price (best = front)
    book_side ask_; ///< ascending by price (best = front)
    std::unordered_map<OrderId, Location> index_;
};

} // namespace order_book
