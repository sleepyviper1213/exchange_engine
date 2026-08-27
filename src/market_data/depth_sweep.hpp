#pragma once
// What a large order would do to the depth a venue publishes.
//
// Depth is only interesting because of the question it answers, and the
// question is not "how many levels are there". It is: if I have to take Q now,
// how much of it is actually there, how far through the book does it reach, and
// what does the reach cost me. A top-of-book quote cannot answer any of that -
// `best_ask` is the same number whether one lot rests behind it or a million.

#include "orders/types.hpp" // side_t
#include "types.hpp"        // scaled_price_t / scaled_qty_t

#include <cstddef>

namespace exchange::market_data {

/**
 * @brief The result of walking one side of a book to fill a given size.
 *
 * @par Why this is not one number
 * Because a taker has three separate problems and they fail independently. The
 * size may simply not be there (@c filled below @c requested, and @c
 * is_complete false) - a book that shows 40 levels can still be thin. The size
 * may be there but only by reaching prices that make the trade pointless (@c
 * impact). And the reach itself is what the venue will publish afterwards: a
 * sweep that consumes @c levels levels moves the touch to @c last, which is the
 * market everyone else then sees. Collapsing those into an average price hides
 * exactly the case worth avoiding, which is a fill that completes at a price
 * nobody wanted.
 *
 * @par What is deliberately absent: a notional
 * There is no Σ price × size here, and it is not an omission. In this book both
 * factors are *scaled decimals* - at a scale of 8, a five-figure price is
 * ~10^12 and a size can be ~10^8 - so their product reaches ~10^20 and a 64-bit
 * accumulator wraps. Wrapping silently on a deep book is precisely the failure
 * this codebase refuses elsewhere (@see volume_t), and there is no 128-bit
 * integer available on every toolchain this builds with. A caller that wants a
 * cost knows its own scales, and can accumulate the levels it gets from
 * @c l2_book::bid_levels / @c ask_levels in whatever type its own units need.
 * The engine's own book has no such problem - prices there are ticks and sizes
 * are lots, which multiply comfortably inside 64 bits - so
 * @c engine::sweep_estimate does carry a notional. That asymmetry is the unit
 * systems being honest about themselves rather than an inconsistency.
 *
 * @note A snapshot of a book between events, like every other read here. It
 *       describes what would happen against the depth published right now, and
 *       says nothing about what a venue would actually do with the order -
 *       hidden size, an order arriving first, and a level cancelled in the
 *       meantime are all outside what a depth feed carries.
 */
struct depth_sweep {
	/// @brief The side that was consumed - @c ask for a buyer, @c bid for a
	///        seller. Carried so the struct reads correctly on its own, and so
	///        @c impact knows which way prices move.
	side_t side;

	/// @brief The size asked for.
	scaled_qty_t requested;

	/// @brief The size the published depth can actually supply, which is
	///        @c requested unless the side ran out of levels first.
	scaled_qty_t filled;

	/// @brief Best price on @p side before the sweep - the touch it starts at.
	///        Meaningless when @c has_liquidity is false.
	scaled_price_t touch;

	/// @brief The worst price the sweep has to reach to fill @c filled, and
	///        therefore where the touch ends up. Equals @c touch when the whole
	///        size fits at the front.
	scaled_price_t last;

	/// @brief Levels consumed, whole or in part. The count a market-impact
	///        estimate is usually stated in, and 0 when the side was empty.
	std::size_t levels;

	/// @brief Did the book have the whole size?
	[[nodiscard]] constexpr bool is_complete() const noexcept {
		return filled == requested;
	}

	/// @brief Was there anything on this side at all? When false, @c touch and
	///        @c last carry no meaning and every other field is zero.
	[[nodiscard]] constexpr bool has_liquidity() const noexcept {
		return levels != 0;
	}

	/**
	 * @brief How far the sweep pushes the touch, as a non-negative price
	 *        distance.
	 *
	 * Signs are resolved here rather than left to the caller: a buyer walks the
	 * asks upwards and a seller walks the bids downwards, so the raw difference
	 * changes sign with the side while the thing being measured - how much
	 * worse the price got - does not.
	 */
	[[nodiscard]] constexpr scaled_price_t impact() const noexcept {
		return side == side_t::ask ? last - touch : touch - last;
	}

	bool operator==(const depth_sweep &) const noexcept = default;
};

} // namespace exchange::market_data
