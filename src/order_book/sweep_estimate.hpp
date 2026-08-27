#pragma once
// What taking a given size out of this book would cost, before taking it.
//
// The aggressive counterpart to queue_position: that one answers "if a sweep
// arrives, what do I get" for an order already resting, and this one answers
// "if I sweep, what do I pay". Both are the same walk over the same ladder from
// opposite ends of the trade.

#include "fwd.hpp"
#include "orders/types.hpp"

#include <cstddef>

namespace exchange::engine {

/**
 * @brief The result of walking one side of the book to fill a given size.
 *
 * @par Four numbers, because a taker has four separate problems
 * The size may not be there at all (@c filled below @c requested). It may be
 * there only at prices that make the trade pointless (@c impact). Getting it
 * may consume a market other participants were relying on (@c levels, and where
 * @c last leaves the touch). And the whole thing has a price, which is @c
 * notional. An average price alone hides the first and third entirely.
 *
 * @par The notional's units, and why it is an exact integer
 * Ticks times lots. Not a currency amount - converting to one needs the
 * listing's scales, which live on @c symbol_spec and deliberately not here -
 * but exactly proportional to one, so comparing two sweeps of the same listing
 * needs no conversion at all. Integer throughout, because the alternative is a
 * double and a double cannot say whether two sweeps cost the same.
 *
 * The product cannot overflow @c volume_t for any book that fits in memory: a
 * price is at most ~4.3e9 ticks and @c filled is bounded by the lots actually
 * resting, so reaching 9.2e18 would need a book holding ~2e9 lots at the top of
 * the tick domain. @c market_data::depth_sweep has no notional for exactly this
 * reason inverted - its factors are scaled decimals and *do* reach 1e20.
 *
 * @note A snapshot. It describes the book as it stands, and the sweep it
 *       describes is the one that would happen if nothing arrived first -
 *       which, on a live venue, is the assumption most likely to be wrong.
 */
struct sweep_estimate {
	/// @brief The side consumed - @c ask for a buyer, @c bid for a seller.
	side_t side;

	/// @brief Lots asked for.
	volume_t requested;

	/// @brief Lots the book can actually supply. Below @c requested when the
	///        crossing depth runs out.
	volume_t filled;

	/// @brief Σ price × quantity over the levels consumed, in tick-lots.
	///        @see the class note on the unit and its range.
	volume_t notional;

	/// @brief Best price on @c side before the sweep. Meaningless when
	///        @c has_liquidity is false.
	price_t touch;

	/// @brief Worst price the sweep reaches, and therefore where it leaves the
	///        touch. Equal to @c touch when the size fits at the front.
	price_t last;

	/// @brief Levels consumed, whole or in part.
	std::size_t levels;

	/// @brief Did the book have the whole size?
	[[nodiscard]] constexpr bool is_complete() const noexcept {
		return filled == requested;
	}

	/// @brief Was there any crossing depth at all? When false every other field
	///        is zero and the prices mean nothing.
	[[nodiscard]] constexpr bool has_liquidity() const noexcept {
		return levels != 0;
	}

	/// @brief How far the sweep moves the touch, as a non-negative distance in
	///        ticks. Signs are resolved here: a buyer walks up, a seller down.
	[[nodiscard]] constexpr price_t impact() const noexcept {
		return side == side_t::ask ? last - touch : touch - last;
	}

	/**
	 * @brief What the sweep costs above filling the whole size at the touch, in
	 *        tick-lots - slippage, as an exact integer.
	 *
	 * Zero when everything filled at the touch, and it is the number that says
	 * whether depth was a problem: @c impact reports how far the *last* lot
	 * reached, which one thin level at the end can dominate, while this weights
	 * every lot by what it actually paid.
	 *
	 * Sign-resolved by side like @c impact, and for the same reason: a seller's
	 * prices get worse going *down*, so the raw difference would come back
	 * negative for exactly the sweeps that hurt most.
	 */
	[[nodiscard]] constexpr volume_t slippage() const noexcept {
		const volume_t at_touch = static_cast<volume_t>(touch) * filled;
		return side == side_t::ask ? notional - at_touch : at_touch - notional;
	}

	bool operator==(const sweep_estimate &) const noexcept = default;
};

} // namespace exchange::engine
