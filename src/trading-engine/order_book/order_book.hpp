#pragma once
#include "trading_engine_export.hpp" // TRADING_ENGINE_EXPORT (generated)
#include "detail/book_side.hpp"
#include "detail/order_location.hpp"
#include "fwd.hpp"
#include "outcome.hpp"
#include "core/util/function_ref.hpp"
#include "resting_view.hpp"

#include <boost/unordered/unordered_flat_map.hpp>

#include <optional>
#include <vector>

namespace exchange::engine {

/**
 * @brief Price-time-priority matching engine for one instrument.
 *
 * @par The three layers
 * ```
 * [ order pool ]  one dense block of cells; every resting order is one cell
 *       ^
 *       | linked by hooks inside the cells themselves
 *       v
 * [ price ladder ]  intrusive tree of levels, best at begin(), plus a flat
 *       |           price->level map for exact lookup
 *       v
 * [ order_list ]  the FIFO at each level: head is oldest and fills first
 * ```
 * Nothing on the matching path allocates once the pools have warmed. Resting an
 * order takes a cell and links it; a fill unlinks one and gives the cell back;
 * a level appears and disappears the same way. Cancel is one flat-map probe for
 * the order's location and an O(1) splice, because the location holds the node
 * itself and the node holds its own links - no side is searched, no level is
 * scanned.
 *
 * @par Entry points
 * - place_order:  matching entry point (crosses, then rests the remainder)
 * - cancel_order: cancel a resting order by id via the id→location index
 * - add_order:    rest anonymous liquidity, no matching (seed/benchmark helper)
 * - delete_order: reduce resting quantity at a price, FIFO-first
 *
 * @par Outputs
 * Matching produces two streams and both matter. @c trade says an execution
 * happened and at what price; @c order_outcome says what became of a named
 * order. They are not redundant - an order can end without ever trading (a
 * rejected fill-or-kill, a dropped IOC remainder, a cancel). Every identified
 * order that reaches @c place_order produces at least one outcome, and every
 * cancel request produces exactly one.
 *
 * @par Threading
 * Single-threaded by contract, and deliberately so: the pools, the ladder and
 * the index are all unsynchronised, and a lock here would be a lock in the
 * matching loop. One book belongs to one matching thread, pinned to one core.
 *
 * @note This is an order-by-order (L3) book only. It has no absolute-size "set
 *       this level to N" primitive, because a venue's L2 diff feed carries no
 *       order identity and applying one here would rest synthetic orders with
 *       invented FIFO position that @c cancel_order cannot see. That
 *       reconstruction path is @c market_data::l2_book, in a library this one
 *       does not link.
 */
class order_book {
public:
	/**
	 * @brief Construct an order book.
	 * @param capacity Expected number of simultaneously resting orders. The
	 *        node pool takes one block of it up front, so a book that stays
	 *        within the hint never asks the allocator for anything again;
	 *        exceeding it is correct but chains another block.
	 */
	TRADING_ENGINE_EXPORT explicit order_book(std::size_t capacity = 1U << 15);

	/**
	 * @brief Matching entry point: validate @p incoming, cross it against the
	 *        opposite side, then rest the unfilled remainder per its
	 *        time-in-force.
	 *
	 * Fills are appended to @p trades and lifecycle records to @p outcomes
	 * (neither is cleared) so a matching engine can accumulate a whole drain
	 * into one pair of reused buffers.
	 *
	 * @par What arrives on @p outcomes
	 * - Validation failure - one REJECTED, and the book is untouched. An order
	 *   is refused for a non-positive quantity (there is no representable
	 *   @c order_state for one), for an unsupported type, or for an id already
	 *   resting (accepting it would overwrite the index entry, orphaning the
	 *   first order's node and making it uncancellable).
	 * - FILL_OR_KILL that cannot be filled in full right now - one REJECTED
	 *   with INSUFFICIENT_LIQUIDITY, and nothing executes.
	 * - Otherwise ACCEPTED, then one FILL per execution *for each side of it*:
	 *   the aggressor and the resting order each get their own record carrying
	 *   their own cumulative quantities, because a client tracking one order
	 *   should not have to reconstruct its position from the trade print.
	 * - An IMMEDIATE_OR_CANCEL remainder - one CANCELLED with TIME_IN_FORCE. A
	 *   GOOD_TILL_CANCELLED remainder simply rests; the ACCEPTED already said
	 *   so.
	 * - A GOOD_TILL_CANCELLED remainder the pools have no cell for - one
	 *   CANCELLED with BOOK_AT_CAPACITY. Whatever crossed still stands: the
	 *   trades are printed and the fills reported, and only the part that
	 *   could not be rested is withdrawn.
	 *
	 * Anonymous orders (id 0) produce no outcomes: there is no one to report to
	 * and no index entry to key them by.
	 */
	TRADING_ENGINE_EXPORT void place_order(const orders::order &incoming,
										   std::vector<trade> &trades,
										   std::vector<order_outcome> &outcomes);

	/**
	 * @brief Convenience overload that discards the outcome stream.
	 * @warning Test and benchmark convenience only. Discarding outcomes throws
	 *          away every non-trade fate an order can have - a rejected FOK and
	 *          a fully-filled one become indistinguishable. Production callers
	 *          take the three-argument form.
	 */
	TRADING_ENGINE_EXPORT void place_order(const orders::order &incoming,
										   std::vector<trade> &trades);

	/// @brief Convenience overload: match @p incoming and return its fills.
	/// @warning Discards outcomes; see the two-argument overload.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::vector<trade>
	place_order(const orders::order &incoming);

	/**
	 * @brief Rest anonymous liquidity at a price without matching.
	 *
	 * Seed/benchmark helper: the order carries no identity (not tracked for
	 * cancel-by-id), no crossing check is performed, and no outcome is emitted.
	 */
	TRADING_ENGINE_EXPORT void add_order(side_t side, price_t price,
										 quantity_t volume);

	/**
	 * @brief Cancel a previously placed (identified) order.
	 *
	 * Exactly one outcome is appended to @p outcomes, and which one is the
	 * cancel/fill race: CANCELLED if the order was still resting, or
	 * CANCEL_REJECTED with UNKNOWN_ORDER if it was not. The second case covers
	 * an order that filled between the client sending the cancel and the book
	 * applying it, one already cancelled, and one that never existed - the
	 * index cannot tell them apart, so neither does the report.
	 *
	 * A cancelled order keeps its executed quantity: the outcome carries the
	 * traded total, and the remainder is what is withdrawn.
	 *
	 * @param id Identifier of the order to cancel.
	 * @param outcomes Buffer the record is appended to; never cleared.
	 */
	TRADING_ENGINE_EXPORT void cancel_order(order_id_t id,
											std::vector<order_outcome> &outcomes);

	/// @brief Convenience overload that discards the outcome.
	/// @warning Test and benchmark convenience only - this is the call whose
	///          silence the lifecycle stream exists to rule out.
	TRADING_ENGINE_EXPORT void cancel_order(order_id_t id);

	/**
	 * @brief Reduce **anonymous** resting quantity at a price, draining whole
	 *        orders FIFO-first.
	 *
	 * The counterpart to @c add_order, and it removes only what that put there.
	 * An identified order sitting at the same price is walked past, not drained:
	 * it belongs to a client, it is withdrawn by @c cancel_order, and that is the
	 * call that produces the CANCELLED record the client is owed. A reduction
	 * carries no identity and emits no outcome, so draining one here would
	 * destroy an order silently - leaving a live entry in whatever record store
	 * sits above the book and nothing to say the order had gone. The same class
	 * of bug as the @c set_level use-after-free this helper outlived.
	 *
	 * A reduction that runs out of anonymous depth before it is satisfied simply
	 * removes what it found; there is no error, because there is nobody to
	 * report one to.
	 *
	 * @param side Book side.
	 * @param price Price level to reduce.
	 * @param volume Quantity to remove. @c volume_t, because a reduction spans
	 *        however many orders it takes to satisfy and is not bounded by any
	 *        one of them.
	 */
	TRADING_ENGINE_EXPORT void delete_order(side_t side, price_t price,
											volume_t volume);

	/**
	 * @brief Drop every resting order on both sides and empty the id index.
	 *
	 * Returns the book to the state its constructor left it in without paying
	 * for one: levels and order nodes go back to the pools they came from, and
	 * the pools keep their blocks, so the first order rested afterwards still
	 * finds a warm cell. Constructing a fresh book instead takes the pool block
	 * and both ladders' level cells again, which is the expensive part.
	 *
	 * @warning Not a mass cancel. Every order simply ceases to exist here - no
	 *          @c order_outcome is emitted, no CANCELLED is reported, and a
	 *          client with a live order learns nothing. Withdrawing a real
	 *          market means @c cancel_order per order, which is what produces
	 *          the records a client is owed. This is for a book being reset
	 *          between sessions, replays or benchmark iterations, where there is
	 *          no one to report to.
	 */
	TRADING_ENGINE_EXPORT void clear() noexcept;

	/**
	 * @brief Aggregate resting quantity at a price on a side.
	 * @param price Price level to query.
	 * @param side Book side.
	 * @return The total resting quantity, or 0 if the level does not exist.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT volume_t
	volume_at_price(price_t price, side_t side) const;

	/// @brief Best (highest) bid price, or std::nullopt if no bids rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<price_t> best_bid() const;

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<price_t> best_ask() const;

	/**
	 * @brief Visit every resting order, in the order it would fill.
	 *
	 * @param visit Invoked as @c visit(const resting_view&) once per resting
	 *        order. Bids first, then asks; within a side best price first; within
	 *        a level oldest first.
	 *
	 * @par Why this exists, having been deliberately withheld
	 * The book has had no way to walk its levels, and that was a decision rather
	 * than an omission - @c detail::book_side's iterators are @c detail, and
	 * @c format.hpp records turning them down for a depth ladder on the grounds
	 * that "widening the book's public surface is a bigger decision than a
	 * formatter should make on its own". Persistence is the reason that decision
	 * finally gets made: a snapshot is a description of exactly this state, and
	 * nothing but the book can produce one.
	 *
	 * What is widened is kept to the minimum that buys it. This yields *values*,
	 * so no caller learns that a level is a pool cell or that an order is a list
	 * node; it is read-only, so nothing can reorder a level by walking it; and it
	 * is a visitor rather than an iterator pair, so the traversal order is the
	 * book's to guarantee rather than the caller's to reconstruct.
	 *
	 * @par The order is the contract
	 * Price-time priority, spelled out - and @c restore_order appends, so feeding
	 * this traversal's output straight back through it reproduces every level's
	 * FIFO exactly. That round trip is the whole point; an unspecified order here
	 * would make a snapshot restore the right *orders* into the wrong *queue*,
	 * which is a book that matches the same flow differently.
	 *
	 * @note Anonymous liquidity (id zero, from @c add_order) is included. It is
	 *       real resting depth and a snapshot that dropped it would restore a
	 *       thinner book than it saved.
	 *
	 * @par Why the visitor is type-erased rather than a template parameter
	 * Because a header template walking this book would have to reach
	 * @c detail::resting_order and a private accessor, and a consumer outside the
	 * shared library cannot instantiate that unless both are exported - which is
	 * precisely the @c detail/ that must not be in the ABI. A @c function_ref puts
	 * the walk in a .cpp behind one exported symbol, at the price of one indirect
	 * call per resting order. That price is payable here and nowhere near the
	 * matching path: this runs once per checkpoint, beside a file write.
	 */
	using resting_visitor = core::util::function_ref<void(const resting_view &) const>;

	TRADING_ENGINE_EXPORT void for_each_resting(resting_visitor visit) const;

	/**
	 * @brief Rest @p order exactly as it was, without matching anything.
	 *
	 * The counterpart to @c for_each_resting, and the other half of a snapshot
	 * round trip. Loading a snapshot through @c place_order instead would be
	 * wrong twice over: the orders would cross each other on the way in (a
	 * snapshot holds both sides of a book that was not crossed, but they arrive
	 * one at a time, and the first bid meets an ask that is already resting), and
	 * each order's cumulative traded quantity would be reset to zero. This
	 * bypasses matching entirely, which a snapshot is entitled to do because the
	 * state it describes was reached by matching already.
	 *
	 * @param order Where it rested and how far through its life it was.
	 * @return @c false if it could not be restored - a duplicate id, an order
	 *         with nothing left to rest, or exhausted pools. Nothing is emitted
	 *         either way: there is no client waiting on a recovery.
	 *
	 * @warning Not for order flow. It writes a book without producing a trade or
	 *          an outcome, which is exactly what recovery wants and exactly what
	 *          a venue must never do to a live order.
	 */
	TRADING_ENGINE_EXPORT bool restore_order(const resting_view &order);

private:
	static constexpr order_id_t kAnonymous = 0; ///< reserved: not indexed

	/// @brief Would a @p side order at @p price trade against @p book_price?
	static bool is_price_crossing(side_t side, price_t price,
								  price_t book_price);

	detail::book_side &side_levels(side_t s);
	[[nodiscard]] const detail::book_side &side_levels(side_t s) const;

	/// @brief Reject @p incoming if it cannot be admitted, appending the record.
	/// @return true when an outcome was emitted and the caller must stop.
	bool reject_if_invalid(const orders::order &incoming,
						   std::vector<order_outcome> &outcomes) const;

	/// @brief Drop the fully-filled head of @p level, clearing its index entry.
	void pop_front(price_level &level);

	/// @brief True if @p volume can be fully filled against @p opposite now.
	[[nodiscard]] bool can_fully_fill(const detail::book_side &opposite,
									  side_t side, price_t price,
									  volume_t volume) const;

	/// Declared before the sides: both bind a reference to it at construction,
	/// and members initialise in declaration order.
	detail::order_pool pool_;
	detail::book_side bid_; ///< descending by price (best = front)
	detail::book_side ask_; ///< ascending by price (best = front)
	boost::unordered_flat_map<order_id_t, detail::order_location> index_;
};

} // namespace exchange::engine
