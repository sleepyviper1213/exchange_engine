#pragma once
#include "detail/book_side.hpp"
#include "fwd.hpp"
#include "outcome.hpp"

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
 *
 * @par Outputs
 * Matching produces two streams, and both matter. @c Trade says an execution
 * happened and at what price; @c OrderOutcome says what became of a named
 * order. They are not redundant — an order can end without ever trading (a
 * rejected fill-or-kill, a dropped IOC remainder, a cancel), and those are
 * exactly the fates that were unobservable while @c Trade was the only output.
 * Every identified order that reaches @c place_order produces at least one
 * outcome, and every cancel request produces exactly one.
 *
 * @note This is an order-by-order (L3) book only. It has no absolute-size
 *       "set this level to N" primitive, because a venue's L2 diff feed carries
 *       no order identity and applying one here would rest synthetic orders
 *       with invented FIFO position that @c cancel_order cannot see. That
 *       reconstruction path is @c market_data::l2_book, in a library this one
 *       does not link.
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
	 * @brief Matching entry point: validate @p incoming, cross it against the
	 *        opposite side, then rest the unfilled remainder per its
	 *        time-in-force.
	 *
	 * Fills are appended to @p trades and lifecycle records to @p outcomes
	 * (neither is cleared) so the matching engine can accumulate a whole drain
	 * into one pair of reused buffers.
	 *
	 * @par What arrives on @p outcomes
	 * - Validation failure — one REJECTED, and the book is untouched. The order
	 *   is refused for a non-positive quantity (there is no representable
	 *   @c order_state for one) or for an id already resting (accepting it
	 *   would overwrite the index entry, orphaning the first order's node and
	 *   making it uncancellable).
	 * - FILL_OR_KILL that cannot be filled in full right now — one REJECTED
	 *   with INSUFFICIENT_LIQUIDITY, and nothing executes.
	 * - Otherwise ACCEPTED, then one FILL per execution *for each side of it*:
	 *   the aggressor and the resting order each get their own record carrying
	 *   their own cumulative quantities, because a client tracking one order
	 *   should not have to reconstruct its position from the trade print.
	 * - An IMMEDIATE_OR_CANCEL remainder — one CANCELLED with TIME_IN_FORCE. A
	 *   GOOD_TILL_CANCELLED remainder simply rests; the ACCEPTED already said so.
	 *
	 * Anonymous orders (id 0) produce no outcomes: there is no one to report to
	 * and no index entry to key them by.
	 */
	TRADING_ENGINE_EXPORT void place_order(const order &incoming,
										   std::vector<Trade> &trades,
										   std::vector<OrderOutcome> &outcomes);

	/**
	 * @brief Convenience overload that discards the outcome stream.
	 * @warning Test and benchmark convenience only. Discarding outcomes throws
	 *          away every non-trade fate an order can have — a rejected FOK and
	 *          a fully-filled one become indistinguishable. Production callers
	 *          take the three-argument form.
	 */
	TRADING_ENGINE_EXPORT void place_order(const order &incoming,
										   std::vector<Trade> &trades);

	/// @brief Convenience overload: match @p incoming and return its fills.
	/// @warning Discards outcomes; see the two-argument overload.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::vector<Trade>
	place_order(const order &incoming);

	/**
	 * @brief Rest anonymous liquidity at a price without matching.
	 *
	 * Seed/benchmark helper: the order carries no identity (not tracked for
	 * cancel-by-id), no crossing check is performed, and no outcome is emitted.
	 */
	TRADING_ENGINE_EXPORT void add_order(side_t side, price_t price, quantity_t volume);

	/**
	 * @brief Cancel a previously placed (identified) order.
	 *
	 * Exactly one outcome is appended to @p outcomes, and which one is the
	 * cancel/fill race: CANCELLED if the order was still resting, or
	 * CANCEL_REJECTED with UNKNOWN_ORDER if it was not. The second case covers
	 * an order that filled between the client sending the cancel and the book
	 * applying it, one already cancelled, and one that never existed — the
	 * index cannot tell them apart, so neither does the report.
	 *
	 * A cancelled order keeps its executed quantity: the outcome carries the
	 * traded total, and the remainder is what is withdrawn.
	 *
	 * @param id Identifier of the order to cancel.
	 * @param outcomes Buffer the record is appended to; never cleared.
	 */
	TRADING_ENGINE_EXPORT void cancel_order(order_id_t id,
											std::vector<OrderOutcome> &outcomes);

	/// @brief Convenience overload that discards the outcome.
	/// @warning Test and benchmark convenience only — this is the call whose
	///          silence Emporia's lifecycle model exists to rule out.
	TRADING_ENGINE_EXPORT void cancel_order(order_id_t id);


	/**
	 * @brief Reduce resting qty at a price, draining whole orders
	 * FIFO-first.
	 * @param side Book side.
	 * @param price Price level to reduce.
	 * @param qty Quantity to remove.
	 */
	TRADING_ENGINE_EXPORT void delete_order(side_t side, price_t price,
											quantity_t volume);

	/**
	 * @brief Aggregate resting qty at a price on a side.
	 * @param price Price level to query.
	 * @param side Book side.
	 * @return The total resting qty, or 0 if the level does not exist.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity_t volume_at_price(price_t price,
															   side_t side) const;

	/// @brief Best (highest) bid_ price, or std::nullopt if no bids rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<price_t> best_bid() const;

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<price_t> best_ask() const;

private:
	/// @brief Where a live order sits, for cancel by id.
	///
	/// The node index is what makes cancel O(1): side and price find the level
	/// in O(log n), and the node then splices straight out of that level's FIFO
	/// with no scan for the matching id.
	struct Location {
		side_t side;
		price_t price;
		detail::node_index node;
	};

	static constexpr order_id_t kAnonymous =
		0; ///< reserved: not tracked in index_

	/// @brief Would a @p side order at @p price trade against @p book_price?
	static bool is_price_crossing(side_t side, price_t price, price_t book_price);

	detail::book_side &side_levels(side_t s);
	[[nodiscard]] const detail::book_side &side_levels(side_t s) const;

	/// @brief Reject @p incoming if it cannot be admitted, appending the record.
	/// @return true when an outcome was emitted and the caller must stop.
	bool reject_if_invalid(const order &incoming,
						   std::vector<OrderOutcome> &outcomes) const;

	/// @brief Drop the fully-filled front order of @p level, clearing its id
	///        index entry.
	void pop_front(Level &level);

	/// @brief True if @p qty can be fully filled against @p opposite now.
	[[nodiscard]] bool can_fully_fill(const detail::book_side &opposite,
									  side_t side, price_t price,
									  quantity_t volume) const;

	/// Declared before the sides: both bind a reference to it at construction,
	/// and members initialise in declaration order.
	detail::order_pool pool_;
	detail::book_side bid_; ///< descending by price (best = front)
	detail::book_side ask_; ///< ascending by price (best = front)
	std::unordered_map<order_id_t, Location> index_;
};

} // namespace exchange::engine
