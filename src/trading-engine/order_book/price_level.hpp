#pragma once
#include "detail/order_pool.hpp"
#include "detail/resting_order.hpp"
#include "fwd.hpp"
#include "trading-engine/orders/order.hpp"
#include "order_state.hpp"

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set_hook.hpp>

#include <cstddef>

namespace exchange::engine {

namespace detail {

/// @brief The FIFO of orders resting at one price: oldest at @c front(),
///        newest at @c back(), which is price-time priority spelled out.
///
/// Intrusive, so appending an order links a cell that already exists rather
/// than allocating a node to point at one, and removing an order in the middle
/// is a splice between two pointers the node already holds. @c
/// constant_time_size keeps @c size() O(1), which is what lets a level report
/// its depth without walking.
using order_list =
	boost::intrusive::list<resting_order,
						   boost::intrusive::member_hook<
							   resting_order, fifo_hook, &resting_order::hook>,
						   boost::intrusive::constant_time_size<true>>;

/// @brief The link a level carries into its side's price ladder.
///
/// @c optimize_size packs the tree node's colour bit into a pointer, which is
/// what keeps @c Level on one cache line.
using ladder_hook = boost::intrusive::set_member_hook<
	boost::intrusive::link_mode<boost::intrusive::safe_link>,
	boost::intrusive::optimize_size<true>>;

} // namespace detail

/**
 * @brief One price level: a price, the FIFO of orders resting at it, and the
 *        aggregate they add up to.
 *
 * A level owns no storage. Its orders are cells of the book's pool threaded
 * through their own hooks, and the level itself is a pool cell threaded into
 * its side's ladder through @c ladder — which is why levels never move, and why
 * a pointer to one stays good for as long as somebody rests at that price.
 *
 * @c volume is maintained incrementally rather than summed on demand. A
 * fill-or-kill has to ask every crossing level how much it holds before
 * executing anything, so a summing implementation would make that check O(all
 * resting orders) instead of O(crossing levels).
 */
struct price_level {
	/// @brief The price, in ticks. Also the ladder's sort key and the map key
	///        that finds this level, so it is fixed for the level's life.
	price_t price;

	/// @brief Resting orders, oldest first. The head is what fills next.
	detail::order_list orders;

	/// @brief Unexecuted quantity across @c orders. O(1) by construction.
	quantity_t volume = 0;

	/// @brief This level's link into its side's ladder. Structural state,
	///        owned by @c detail::book_side.
	detail::ladder_hook ladder;

	/// @brief Rest @p order at this level, drawing its node from @p pool.
	/// @return The node it rested in, or @c nullptr if @p pool had no cell
	///         left, in which case the level is unchanged.
	TRADING_ENGINE_EXPORT detail::resting_order *
	add_order(detail::order_pool &pool, const orders::order &order);

	/// @brief Rest an order that already has a lifecycle — an aggressor's
	///        unfilled remainder — so the node continues @p state rather than
	///        starting a fresh one. @see detail::resting_order
	TRADING_ENGINE_EXPORT detail::resting_order *
	add_order(detail::order_pool &pool, order_id_t id,
			  const order_state &state);

	/// @brief True when no orders rest at this level.
	[[nodiscard]] TRADING_ENGINE_EXPORT bool has_empty_orders() const noexcept;

	/// @brief Sum of the resting orders' unexecuted quantities. O(1).
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity_t
	total_volume() const noexcept;

	/// @brief How many orders rest here. O(1).
	///
	/// Exported alongside @c total_volume so out-of-DLL readers (the formatter)
	/// go through @c Level rather than reaching into the intrusive list.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t
	order_count() const noexcept;

	/// @brief The oldest resting order — the one that fills next.
	/// @pre The level is not empty.
	[[nodiscard]] detail::resting_order &front() noexcept;

	/// @brief Execute @p amount against the head order, keeping @c volume in
	///        step. The head stays where it is; a partial fill does not cost an
	///        order its place in the queue.
	void fill_front(quantity_t amount) noexcept;

	/// @brief Drop the head order and return its cell to @p pool.
	/// @pre The level is not empty. The caller has already read whatever the
	///      node owes an outcome report, and dropped its index entry.
	void pop_front(detail::order_pool &pool) noexcept;

	/// @brief Splice @p node out wherever it sits and return its cell — the
	///        cancel path, and the reason cancelling is O(1): the node carries
	///        its own links, so nothing is searched for.
	void unlink(detail::order_pool &pool, detail::resting_order &node) noexcept;

	/// @brief Return every node still resting here to @p pool, leaving the
	///        level empty. What a side does before discarding a level.
	void release_orders(detail::order_pool &pool) noexcept;
};

// price + list header + aggregate + ladder hook. Levels are what the matching
// loop and the ladder walk, so one of them should cost one line, not two.
static_assert(sizeof(price_level) <= 64, "a Level must not outgrow a cache line");

} // namespace exchange::engine
