#pragma once
#include "../fwd.hpp"
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
 */
class book_side {
public:
	TRADING_ENGINE_EXPORT explicit book_side(Side side) noexcept;

	[[nodiscard]] TRADING_ENGINE_EXPORT bool empty() const noexcept;

	/// @brief Best resting price, or std::nullopt when the side is empty.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::optional<Price> best_price() const;

	/// @brief The best (front) level. Precondition: !empty().
	[[nodiscard]] TRADING_ENGINE_EXPORT Level &best();
	[[nodiscard]] TRADING_ENGINE_EXPORT const Level &best() const;

	/// @brief The level resting at exactly @p price, or nullptr if none.
	[[nodiscard]] TRADING_ENGINE_EXPORT Level *find(Price price);
	[[nodiscard]] TRADING_ENGINE_EXPORT const Level *find(Price price) const;

	/// @brief Place @p incoming at its price, creating the level in sorted
	///        position if it does not exist yet. Returns the level it landed
	///        in.
	TRADING_ENGINE_EXPORT Level &insert(const Order &incoming);

	TRADING_ENGINE_EXPORT void remove_best_level_if_empty();

	/// @brief Erase the level at @p price outright (no-op if absent).
	TRADING_ENGINE_EXPORT void erase(Price price);

	/// @brief Aggregate resting volume at @p price, or 0 if the level is
	/// absent.
	[[nodiscard]] TRADING_ENGINE_EXPORT Volume
	volume_at_price(Price price) const;

	[[nodiscard]] TRADING_ENGINE_EXPORT std::vector<Level>::const_iterator
	begin() const noexcept;
	[[nodiscard]] TRADING_ENGINE_EXPORT std::vector<Level>::const_iterator
	end() const noexcept;

private:
	/// @brief Sorted position for @p price: the first level not ordered better
	///        than it (bids desc, asks asc).
	[[nodiscard]] std::vector<Level>::iterator lower_bound(Price price);
	[[nodiscard]] std::vector<Level>::const_iterator
	lower_bound(Price price) const;

	Side side_;
	std::vector<Level> levels_;
};

} // namespace exchange::engine::detail
