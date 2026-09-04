#pragma once

#include "order_book_export.hpp" // ORDER_BOOK_EXPORT (generated)
#include "../price_level.hpp"
#include "order_pool.hpp"

#include <boost/intrusive/set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <optional>

namespace exchange::engine::detail {

/**
 * @brief Orders levels best-first: bids descending, asks ascending.
 *
 * Stateful because a side knows which way it sorts and a level does not. The
 * branch is on a member that never changes for the life of the side, so it
 * predicts perfectly; the alternative - a distinct ladder type per side - would
 * make @c order_book unable to name "the side this order joins" at run time.
 */
struct level_price_order {
	side_t side;

	[[nodiscard]] bool operator()(const price_level &a, const price_level &b) const noexcept {
		return side == side_t::bid ? a.price > b.price : a.price < b.price;
	}
};

/// @brief The price ladder: levels kept in matching order, best at @c begin().
///
/// Intrusive, so a level's position costs nothing beyond the hook it already
/// carries, and - the reason it is not a sorted vector - inserting a price in
/// the middle relinks pointers instead of shifting the levels around it. A
/// level that moved would take its orders' list heads with it and strand every
/// pointer into them.
using ladder = boost::intrusive::set<
	price_level,
	boost::intrusive::member_hook<price_level, ladder_hook, &price_level::ladder>,
	boost::intrusive::compare<level_price_order>,
	boost::intrusive::constant_time_size<false> >;

/**
 * @brief One side of the book: the price ladder, plus the map that finds a
 *        price without walking it.
 *
 * Two views of the same levels, because the book asks two different questions.
 * Matching asks "what is best, and what is next best" - that is the ladder, and
 * it is ordered. Resting and cancelling ask "is there a level at exactly this
 * price" - that is @c by_price_, and an open-addressed flat map answers it with
 * one probe instead of the @c O(log n) pointer chase down the tree.
 *
 * Levels and orders are pool cells: this type owns the level pool and borrows
 * the book's order pool, since both sides draw their nodes from one place so a
 * level's storage does not depend on which side it landed on.
 */
class book_side {
public:
	/// @brief Levels taken in the level pool's first block by default.
	static constexpr std::size_t DEFAULT_LEVEL_CAPACITY = 1U << 10;
	ORDER_BOOK_EXPORT
	book_side(side_t side, order_pool &pool,
			  std::size_t level_capacity = DEFAULT_LEVEL_CAPACITY);

	/// @brief Returns every level and every order still resting to their pools.
	ORDER_BOOK_EXPORT ~book_side();

	/// @brief Drop every level and every order resting on this side.
	///
	/// What the destructor does, without the side ceasing to exist: cells go back
	/// to the pools they came from, and the pools keep their blocks, so a side
	/// emptied this way rests its next order without touching the allocator.
	/// @warning Nodes are released without consulting the book's id→location
	///          index, exactly as @c erase does. @c order_book::clear empties the
	///          index in the same breath; a caller that clears one side alone
	///          must do the same or leave every entry dangling.
	ORDER_BOOK_EXPORT void clear() noexcept;

	// Non-copyable, non-movable: the ladder links point at levels this side owns.
	book_side(const book_side &)            = delete;
	book_side &operator=(const book_side &) = delete;
	book_side(book_side &&)                 = delete;
	book_side &operator=(book_side &&)      = delete;

	[[nodiscard]] ORDER_BOOK_EXPORT bool empty() const noexcept;

	/// @brief Best resting price, or std::nullopt when the side is empty.
	[[nodiscard]] ORDER_BOOK_EXPORT std::optional<price_t>
	best_price() const;

	/// @brief The best level. @pre Not empty.
	[[nodiscard]] ORDER_BOOK_EXPORT price_level &best();
	[[nodiscard]] ORDER_BOOK_EXPORT const price_level &best() const;

	/// @brief The level resting at exactly @p price, or nullptr if none.
	[[nodiscard]] ORDER_BOOK_EXPORT price_level *find(price_t price);
	[[nodiscard]] ORDER_BOOK_EXPORT const price_level *find(price_t price) const;

	/// @brief Rest @p incoming at its price, creating the level if this is the
	///        first order there.
	/// @return The level it landed in - its node is that level's
	///         @c orders.back() - or @c nullptr if a pool was exhausted, in
	///         which case the side is left exactly as it was found.
	ORDER_BOOK_EXPORT price_level *insert(const orders::order &incoming);

	/// @brief Rest @p id at @p price carrying an existing @p state - an
	///        aggressor's unfilled remainder. @see Level::add_order
	ORDER_BOOK_EXPORT price_level *insert(order_id_t id, price_t price,
										const order_state &state);

	/// @brief Drop the best level if the matching loop drained it.
	ORDER_BOOK_EXPORT void remove_best_level_if_empty();

	/// @brief Erase the level at @p price outright (no-op if absent), returning
	///        any nodes still resting on it to the order pool.
	/// @warning Those nodes are released without consulting the book's
	///          id→location index, so erasing a level that still holds
	///          *identified* orders would leave those entries dangling. Every
	///          caller erases a level it has already drained; the release here
	///          is a backstop, not the normal path.
	ORDER_BOOK_EXPORT void erase(price_t price);

	/// @brief Aggregate resting quantity at @p price, or 0 if absent.
	/// @see price_level::volume - a sum across orders, hence @c volume_t.
	[[nodiscard]] ORDER_BOOK_EXPORT volume_t
	volume_at_price(price_t price) const;

	/// @brief Walk the levels best-first - what a fill-or-kill check needs to
	///        add up the liquidity it can reach.
	[[nodiscard]] ORDER_BOOK_EXPORT ladder::const_iterator
	begin() const noexcept;
	
	[[nodiscard]] ORDER_BOOK_EXPORT ladder::const_iterator
	end() const noexcept;

private:
	/// @brief The level at @p price, created in ladder position if absent, or
	///        @c nullptr if the level pool had no cell left.
	[[nodiscard]] price_level *level_at(price_t price);

	/// @brief Undo a level this insert had to create, when the order it was
	///        created for could not be rested after all.
	/// @return Always @c nullptr, so a failing insert reads as one line.
	price_level *rewind(price_level &level) noexcept;

	/// @brief Unlink @p level from both views and return it, and everything
	///        resting on it, to the pools.
	void destroy(price_level &level) noexcept;

	order_pool &pool_; ///< shared with the other side; owned by the order_book
	basic_pool<price_level> levels_;
	ladder ordered_;
	boost::unordered_flat_map<price_t, price_level *> by_price_;
};

} // namespace exchange::engine::detail
