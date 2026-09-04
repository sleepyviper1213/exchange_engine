#pragma once
#include "allocation_policy.hpp"
#include "core/util/function_ref.hpp"
#include "detail/book_side.hpp"
#include "detail/order_location.hpp"
#include "fwd.hpp"
#include "order_book_export.hpp" // ORDER_BOOK_EXPORT (generated)
#include "outcome.hpp"
#include "queue_position.hpp"
#include "sweep_estimate.hpp"

#include <boost/unordered/unordered_flat_map.hpp>

#include <optional>
#include <vector>

namespace exchange::engine {

/**
 * @brief Order-by-order matching engine for one instrument: price first,
 *        then the level's own allocation policy.
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
 * @par Priority, and the one part of it that is a choice
 * Price priority is absolute and not configurable: the best price on the
 * opposite side is what an aggressor reaches first, because the ladder is
 * ordered and the matching loop walks it from @c best(). What a venue gets to
 * choose is the tie-break *within* a price, and this book takes it as a
 * construction parameter - @c allocation_policy::PRICE_TIME hands a partial
 * sweep down the level's FIFO, @c allocation_policy::PRO_RATA divides it by
 * resting size. It is fixed for the book's life, the way a listing's matching
 * algorithm is fixed for a session; a book that changed policy while orders
 * rested would have promised two different things to two orders standing in the
 * same queue. @see allocation_policy for why the choice exists at all.
 *
 * Queue position is readable rather than merely implied: @c queue_position_of
 * says where a resting order stands, and @c projected_fill says what it would
 * receive from a sweep of a given size under whichever policy is in force.
 * Neither is on the matching path. @see queue_position
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
	 * @param policy How a level divides an aggressor that cannot take all of
	 *        it. Immutable afterwards - @see the class note on why.
	 */
	ORDER_BOOK_EXPORT explicit order_book(
		std::size_t capacity     = 1U << 15,
		allocation_policy policy = allocation_policy::PRICE_TIME);

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
	ORDER_BOOK_EXPORT void place_order(const orders::order &incoming,
									   std::vector<trade> &trades,
									   std::vector<order_outcome> &outcomes);

	/**
	 * @brief Convenience overload that discards the outcome stream.
	 * @warning Test and benchmark convenience only. Discarding outcomes throws
	 *          away every non-trade fate an order can have - a rejected FOK and
	 *          a fully-filled one become indistinguishable. Production callers
	 *          take the three-argument form.
	 */
	ORDER_BOOK_EXPORT void place_order(const orders::order &incoming,
									   std::vector<trade> &trades);

	/// @brief Convenience overload: match @p incoming and return its fills.
	/// @warning Discards outcomes; see the two-argument overload.
	[[nodiscard]] ORDER_BOOK_EXPORT std::vector<trade>
	place_order(const orders::order &incoming);

	/**
	 * @brief Rest anonymous liquidity at a price without matching.
	 *
	 * Seed/benchmark helper: the order carries no identity (not tracked for
	 * cancel-by-id), no crossing check is performed, and no outcome is emitted.
	 */
	ORDER_BOOK_EXPORT void add_order(side_t side, price_t price,
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
	ORDER_BOOK_EXPORT void cancel_order(order_id_t id,
										std::vector<order_outcome> &outcomes);

	/// @brief Convenience overload that discards the outcome.
	/// @warning Test and benchmark convenience only - this is the call whose
	///          silence the lifecycle stream exists to rule out.
	ORDER_BOOK_EXPORT void cancel_order(order_id_t id);

	/**
	 * @brief Reduce **anonymous** resting quantity at a price, draining whole
	 *        orders FIFO-first.
	 *
	 * The counterpart to @c add_order, and it removes only what that put there.
	 * An identified order sitting at the same price is walked past, not
	 * drained: it belongs to a client, it is withdrawn by @c cancel_order, and
	 * that is the call that produces the CANCELLED record the client is owed. A
	 * reduction carries no identity and emits no outcome, so draining one here
	 * would destroy an order silently - leaving a live entry in whatever record
	 * store sits above the book and nothing to say the order had gone. The same
	 * class of bug as the @c set_level use-after-free this helper outlived.
	 *
	 * A reduction that runs out of anonymous depth before it is satisfied
	 * simply removes what it found; there is no error, because there is nobody
	 * to report one to.
	 *
	 * @param side Book side.
	 * @param price Price level to reduce.
	 * @param volume Quantity to remove. @c volume_t, because a reduction spans
	 *        however many orders it takes to satisfy and is not bounded by any
	 *        one of them.
	 */
	ORDER_BOOK_EXPORT void delete_order(side_t side, price_t price,
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
	 *          between sessions, replays or benchmark iterations, where there
	 * is no one to report to.
	 */
	ORDER_BOOK_EXPORT void clear() noexcept;

	/**
	 * @brief Aggregate resting quantity at a price on a side.
	 * @param price Price level to query.
	 * @param side Book side.
	 * @return The total resting quantity, or 0 if the level does not exist.
	 */
	[[nodiscard]] ORDER_BOOK_EXPORT volume_t volume_at_price(price_t price,
															 side_t side) const;

	/// @brief Best (highest) bid price, or std::nullopt if no bids rest.
	[[nodiscard]] ORDER_BOOK_EXPORT std::optional<price_t> best_bid() const;

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] ORDER_BOOK_EXPORT std::optional<price_t> best_ask() const;

	/// @brief How this book divides a partial sweep. Fixed at construction.
	[[nodiscard]] ORDER_BOOK_EXPORT allocation_policy policy() const noexcept;

	/**
	 * @brief Where the order @p id stands in the queue at its price.
	 *
	 * @return The snapshot, or @c std::nullopt if no order with that id rests -
	 *         which covers one that filled, one that was cancelled, one that
	 *         never existed, and anonymous liquidity, exactly as
	 *         @c cancel_order cannot tell those apart either.
	 *
	 * @note O(orders ahead of it): a level is a FIFO and an order's rank in one
	 *       is not stored. Deliberately not cached - keeping a rank per node
	 *       would mean touching every node behind a cancel, which is work the
	 *       matching path would pay for a number only a reader wants.
	 */
	[[nodiscard]] ORDER_BOOK_EXPORT std::optional<queue_position>
	queue_position_of(order_id_t id) const;

	/**
	 * @brief Lots the order @p id would receive from an aggressor of
	 *        @p incoming lots priced through its level, right now.
	 *
	 * The question a passive quote actually has - "if a sweep of this size
	 * arrives, do I trade?" - answered by the same arithmetic the matcher runs,
	 * so a strategy's model of the venue is the venue. @see
	 * detail::allocation_for
	 *
	 * The aggressor is assumed priced *through* this order's level, so it pays
	 * for every better level on our side before reaching us: those aggregates
	 * come off @p incoming first, and what is left is what gets divided at our
	 * price under the book's @c policy.
	 *
	 * @return Lots allocated, never more than the order's remaining quantity.
	 *         Zero for an id that is not resting, and zero when the sweep is
	 *         exhausted before it reaches us - an order that gets nothing and
	 *         an order that is not there have the same fill.
	 */
	[[nodiscard]] ORDER_BOOK_EXPORT quantity_t
	projected_fill(order_id_t id, volume_t incoming) const;

	/**
	 * @brief What taking @p lots out of @p side would cost, without taking it.
	 *
	 * Walks @p side from the touch outwards exactly as the matching loop would,
	 * and reports how much of @p lots is really there, how many levels it
	 * reaches through, where it leaves the touch and what it pays.
	 * @see sweep_estimate, which explains why the answer is not one number.
	 *
	 * @param side The side *consumed* - a buyer passes @c side_t::ask. Named
	 *        for the depth rather than the intent, matching @c volume_at_price;
	 *        @c place_order is the one that takes an aggressor's own side.
	 * @param lots Size wanted. Non-positive returns an empty estimate: "take
	 *        nothing" has an answer and it is not an error.
	 *
	 * @par What it does not model
	 * The allocation policy, and it does not need to: a sweep either clears a
	 * level or is the last thing that happens to it, and in both cases the lots
	 * and the prices are the same whoever they are divided between. Which
	 * *orders* fill is `PRO_RATA`'s business; what the taker pays is not.
	 *
	 * It also assumes nothing arrives first, which on a live venue is the
	 * assumption most likely to be wrong - and it charges no fees, since the
	 * book has no fee schedule.
	 *
	 * @note O(levels consumed), and never on the matching path.
	 */
	[[nodiscard]] ORDER_BOOK_EXPORT sweep_estimate
	estimate_sweep(side_t side, volume_t lots) const;

	/**
	 * @brief Visit every resting order, in the order it would fill.
	 *
	 * @param visit Invoked as @c visit(const resting_view&) once per resting
	 *        order. Bids first, then asks; within a side best price first;
	 * 		  within a level oldest first.
	 *
	 * @par Why this exists, having been deliberately withheld
	 * The book has had no way to walk its levels, and that was a decision
	 * rather than an omission - @c detail::book_side's iterators are @c detail,
	 * and
	 * @c format.hpp records turning them down for a depth ladder on the grounds
	 * that "widening the book's public surface is a bigger decision than a
	 * formatter should make on its own". Persistence is the reason that
	 * decision finally gets made: a snapshot is a description of exactly this
	 * state, and nothing but the book can produce one.
	 *
	 * What is widened is kept to the minimum that buys it. This yields
	 * *values*, so no caller learns that a level is a pool cell or that an
	 * order is a list node; it is read-only, so nothing can reorder a level by
	 * walking it; and it is a visitor rather than an iterator pair, so the
	 * traversal order is the book's to guarantee rather than the caller's to
	 * reconstruct.
	 *
	 * @par The order is the contract
	 * Price-time priority, spelled out - and @c restore_order appends, so
	 * feeding this traversal's output straight back through it reproduces every
	 * level's FIFO exactly. That round trip is the whole point; an unspecified
	 * order here would make a snapshot restore the right *orders* into the
	 * wrong *queue*, which is a book that matches the same flow differently.
	 *
	 * @note Anonymous liquidity (id zero, from @c add_order) is included. It is
	 *       real resting depth and a snapshot that dropped it would restore a
	 *       thinner book than it saved.
	 *
	 * @par Why the visitor is type-erased rather than a template parameter
	 * Because a header template walking this book would have to reach
	 * @c detail::resting_order and a private accessor, and a consumer outside
	 * the shared library cannot instantiate that unless both are exported -
	 * which is precisely the @c detail/ that must not be in the ABI. A @c
	 * function_ref puts the walk in a .cpp behind one exported symbol, at the
	 * price of one indirect call per resting order. That price is payable here
	 * and nowhere near the matching path: this runs once per checkpoint, beside
	 * a file write.
	 */
	ORDER_BOOK_EXPORT void for_each_resting(
		core::util::function_ref<void(const resting_view &) const> visit) const;

	/**
	 * @brief Rest @p order exactly as it was, without matching anything.
	 *
	 * The counterpart to @c for_each_resting, and the other half of a snapshot
	 * round trip. Loading a snapshot through @c place_order instead would be
	 * wrong twice over: the orders would cross each other on the way in (a
	 * snapshot holds both sides of a book that was not crossed, but they arrive
	 * one at a time, and the first bid meets an ask that is already resting),
	 * and each order's cumulative traded quantity would be reset to zero. This
	 * bypasses matching entirely, which a snapshot is entitled to do because
	 * the state it describes was reached by matching already.
	 *
	 * @param order Where it rested and how far through its life it was.
	 * @return @c false if it could not be restored - a duplicate id, an order
	 *         with nothing left to rest, or exhausted pools. Nothing is emitted
	 *         either way: there is no client waiting on a recovery.
	 *
	 * @warning Not for order flow. It writes a book without producing a trade
	 * or an outcome, which is exactly what recovery wants and exactly what a
	 * venue must never do to a live order.
	 */
	ORDER_BOOK_EXPORT bool restore_order(const resting_view &order);

private:
	static constexpr order_id_t ANONYMOUS = 0; ///< reserved: not indexed

	/// @brief Would a @p side order at @p price trade against @p book_price?
	static bool is_price_crossing(side_t side, price_t price,
								  price_t book_price);

	detail::book_side &side_levels(side_t s);
	[[nodiscard]] const detail::book_side &side_levels(side_t s) const;

	/// @brief Reject @p incoming if it cannot be admitted, appending the
	/// record.
	/// @return true when an outcome was emitted and the caller must stop.
	bool reject_if_invalid(const orders::order &incoming,
						   std::vector<order_outcome> &outcomes) const;

	/// @brief Drop the fully-filled head of @p level, clearing its index entry.
	void pop_front(price_level &level);

	/// @brief Drop @p node from @p level wherever it sits, clearing its index
	///        entry. What @c pop_front does for a node that is not the head -
	///        which only pro-rata produces, since price-time matching never
	///        fills anything but the head.
	void remove_order(price_level &level, detail::resting_order &node);

	/// @brief Cross @p aggressor against @p level oldest-first, appending a
	///        trade and its two fill records per resting order it consumes.
	///
	/// The path for @c PRICE_TIME, and also for a @c PRO_RATA sweep that takes
	/// the whole level: when every resting order fills in full there is nothing
	/// to divide, and this loop is the cheaper way to say so.
	void cross_time_priority(price_level &level, const orders::order &incoming,
							 order_state &aggressor, std::vector<trade> &trades,
							 std::vector<order_outcome> &outcomes);

	/// @brief Divide @p aggressor across every order at @p level in proportion
	///        to what each has resting, settling the rounding residual by time
	///        priority.
	///
	/// @pre @c aggressor.remaining() < @c level.total_volume() - the full-sweep
	///      case belongs to @c cross_time_priority, and the share arithmetic
	///      assumes the partial one. @see detail::pro_rata_share
	void cross_pro_rata(price_level &level, const orders::order &incoming,
						order_state &aggressor, std::vector<trade> &trades,
						std::vector<order_outcome> &outcomes);

	/// @brief True if @p volume can be fully filled against @p opposite now.
	[[nodiscard]] bool can_fully_fill(const detail::book_side &opposite,
									  side_t side, price_t price,
									  volume_t volume) const;

	/// Declared before the sides: both bind a reference to it at construction,
	/// and members initialise in declaration order.
	allocation_policy policy_;
	detail::order_pool pool_;
	detail::book_side bid_; ///< descending by price (best = front)
	detail::book_side ask_; ///< ascending by price (best = front)
	boost::unordered_flat_map<order_id_t, detail::order_location> index_;
};

} // namespace exchange::engine
