#pragma once

#include "../level.hpp"

#include <optional>
#include <vector>

namespace exchange::engine::detail {

/**
 * @brief One side of the book: price levels kept sorted best-first.
 *
 * Bids sort descending and asks ascending, so the best price is always
 * front(). This type owns the sorted-vector bookkeeping — lookup, ordered
 * insertion of a new level, and erase-when-empty — so callers deal in whole
 * levels rather than raw vector positions.
 *
 * The resting-order nodes live in a pool owned by the order_book, not here:
 * both sides share one pool so a level's storage does not depend on which side
 * it landed on. The reference is bound once at construction rather than passed
 * per call, because a side is never used with a pool other than its book's.
 */
class book_side {
public:
	TRADING_ENGINE_EXPORT book_side(side_t side, order_pool &pool) noexcept;

	[[nodiscard]] TRADING_ENGINE_EXPORT bool empty() const noexcept;

	/// @brief Best resting price, or std::nullopt when the side is empty.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<price_t> best_price() const;

	/// @brief The best (front) level. Precondition: !empty().
	[[nodiscard]] TRADING_ENGINE_EXPORT Level &best();
	[[nodiscard]] TRADING_ENGINE_EXPORT const Level &best() const;

	/// @brief The level resting at exactly @p price, or nullptr if none.
	[[nodiscard]] TRADING_ENGINE_EXPORT Level *find(price_t price);
	[[nodiscard]] TRADING_ENGINE_EXPORT const Level *find(price_t price) const;

	/// @brief Place @p incoming at its price, creating the level in sorted
	///        position if it does not exist yet. Returns the level it landed
	///        in; the node it was given is that level's @c orders.back().
	TRADING_ENGINE_EXPORT Level &insert(const Order &incoming);

	TRADING_ENGINE_EXPORT void remove_best_level_if_empty();

	/// @brief Erase the level at @p price outright (no-op if absent),
	///        returning any nodes still resting on it to the pool.
	/// @warning Those nodes are freed without consulting the book's id->Location
	///          index, so a caller erasing a level that still holds *identified*
	///          orders must drop their index entries first or they will dangle.
	///          The two callers that erase a non-empty level (the L2 set_level
	///          path) rest only anonymous liquidity, which is never indexed.
	TRADING_ENGINE_EXPORT void erase(price_t price);

	/// @brief Aggregate resting qty at @p price, or 0 if the level is
	/// absent.
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity_t
	volume_at_price(price_t price) const;

	[[nodiscard]] TRADING_ENGINE_EXPORT std::vector<Level>::const_iterator
	begin() const noexcept;
	[[nodiscard]] TRADING_ENGINE_EXPORT std::vector<Level>::const_iterator
	end() const noexcept;

private:
	/// @brief Sorted position for @p price: the first level not ordered better
	///        than it (bids desc, asks asc).
	[[nodiscard]] std::vector<Level>::iterator lower_bound(price_t price);
	[[nodiscard]] std::vector<Level>::const_iterator
	lower_bound(price_t price) const;

	/// @brief Return every node still resting on @p level to the pool.
	void release_nodes(Level &level);

	side_t side_;
	order_pool &pool_; ///< shared with the other side; owned by the order_book
	std::vector<Level> levels_;
};

} // namespace exchange::engine::detail
