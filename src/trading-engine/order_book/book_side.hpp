//
// Created by BZS_TestCode1 on 7/20/2026.
//

#ifndef ORDER_BOOK_BOOK_SIDE_HPP
#define ORDER_BOOK_BOOK_SIDE_HPP
#include "level.hpp"
#include "order.hpp"
#include "side.hpp"
#include "types.hpp"

#include <optional>
#include <vector>

namespace order_book {

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
	explicit book_side(Side side) noexcept;

	[[nodiscard]] bool empty() const noexcept;

	/// @brief Best resting price, or std::nullopt when the side is empty.
	[[nodiscard]] std::optional<Price> best_price() const;

	/// @brief The best (front) level. Precondition: !empty().
	[[nodiscard]] Level &best();
	[[nodiscard]] const Level &best() const;

	/// @brief The level resting at exactly @p price, or nullptr if none.
	[[nodiscard]] Level *find(Price price);
	[[nodiscard]] const Level *find(Price price) const;

	/// @brief Place @p incoming at its price, creating the level in sorted
	///        position if it does not exist yet. Returns the level it landed in.
	Level &insert(const Order &incoming);

	void remove_best_level_if_empty();

	/// @brief Erase the level at @p price outright (no-op if absent).
	void erase(Price price);

	/// @brief Aggregate resting volume at @p price, or 0 if the level is absent.
	[[nodiscard]] Volume volume_at_price(Price price) const;

	[[nodiscard]] std::vector<Level>::const_iterator begin() const noexcept;
	[[nodiscard]] std::vector<Level>::const_iterator end() const noexcept;

private:
	/// @brief Sorted position for @p price: the first level not ordered better
	///        than it (bids desc, asks asc).
	[[nodiscard]] std::vector<Level>::iterator lower_bound(Price price);
	[[nodiscard]] std::vector<Level>::const_iterator
	lower_bound(Price price) const;

	Side side_;
	std::vector<Level> levels_;
};

} // namespace order_book

#endif // ORDER_BOOK_BOOK_SIDE_HPP