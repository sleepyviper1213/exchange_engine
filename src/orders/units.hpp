#pragma once
// The engine's units of measure, as types.
//
// Every price and size in the tree used to be a bare integer, and three
// different integers at that: ticks and lots on the matching grid, the same
// numbers times 10^scale on the feed, and tick-lot products for notionals. Each
// crossing was a cast, and nothing stopped a cast in the wrong direction -
// sweep_estimate once held a tick-lot notional in a lots type, and returned a
// tick *distance* typed as a price *level*. With mp-units those are compile
// errors, and the representation underneath is unchanged: every alias here is
// the size and triviality of the integer it replaced, which the static_asserts
// in types.hpp pin.

// Kept in `orders` rather than `core`: `market_data` already links `orders`,
// so the feed's scaled units can sit next to the engine's without a new edge.

#include <mp-units/framework.h>

#include <climits>
#include <concepts>
#include <cstddef>
#include <functional>

namespace exchange::units {

// --- dimensions -------------------------------------------------------------

inline constexpr struct dim_price final : mp_units::base_dimension<"P"> {
} dim_price;

inline constexpr struct dim_size final : mp_units::base_dimension<"S"> {
} dim_size;

inline constexpr struct dim_currency final : mp_units::base_dimension<"$"> {
} dim_currency;

// --- quantities -------------------------------------------------------------

// QUANTITY_SPEC rather than a hand-written struct: the vcpkg target builds with
// MP_UNITS_API_NO_CRTP=0, where the spec names itself as the first argument,
// and the macro spells whichever form the configuration expects.
QUANTITY_SPEC(price, dim_price);
QUANTITY_SPEC(size, dim_size);
QUANTITY_SPEC(currency, dim_currency);

/// @brief One order's size - a child of @c size, not a synonym for it.
///
/// mp-units does not guard representation narrowing: @c quantity<lot, int64>
/// converts implicitly to @c quantity<lot, int32>. Making the per-order
/// quantity a child spec is what turns @c volume_t -> @c quantity_t into an
/// explicit @c quantity_cast, while the widening direction stays implicit.
QUANTITY_SPEC(order_size, size);

// --- the matching grid ------------------------------------------------------

/// @brief One increment of a listing's price grid. Not a currency amount - its
///        value is @c symbol_spec::tick_scaled, which is per listing.
inline constexpr struct tick final
	: mp_units::named_unit<"tick", mp_units::kind_of<price>> {
} tick;

/// @brief Half a tick: the unit a midpoint is exact in, whatever the spread.
inline constexpr struct half_tick final
	: mp_units::named_unit<"htick", mp_units::mag_ratio<1, 2> * tick> {
} half_tick;

/// @brief One increment of a listing's size grid.
inline constexpr struct lot final
	: mp_units::named_unit<"lot", mp_units::kind_of<size>> {
} lot;

// --- the feed's grid --------------------------------------------------------

/// @brief 10^-price_scale of the quote currency.
///
/// The scale is chosen at run time, so this unit has no compile-time relation
/// to @c tick - and that is the feature: with no common unit, mp-units refuses
/// to add, compare or convert the two. @c symbol_spec, which knows the
/// listing's tick, is the only place a value crosses.
inline constexpr struct scaled_price final
	: mp_units::named_unit<"spx", mp_units::kind_of<price>> {
} scaled_price;

/// @brief 10^-qty_scale of the base asset. @see scaled_price
inline constexpr struct scaled_size final
	: mp_units::named_unit<"ssz", mp_units::kind_of<size>> {
} scaled_size;

// --- money ------------------------------------------------------------------

/// @brief One tether. Its own unit of @c currency, with no conversion to any
///        other: a second quote currency is a second unit, and a sum across
///        the two does not compile until somebody supplies a rate.
inline constexpr struct usdt final
	: mp_units::named_unit<"USDT", mp_units::kind_of<currency>> {
} usdt;

/// @brief 10^-8 USDT, the grain money is counted in.
///
/// Fixed at compile time where @c scaled_price is not, because it is a choice
/// about money rather than a fact about a listing: eight decimals is the finest
/// any Binance spot filter publishes, so every listing whose tick-lot is a
/// whole number of these converts exactly. @see symbol_spec::usdt_from
inline constexpr struct usdt_e8 final
	: mp_units::named_unit<"USDTe8", mp_units::mag_power<10, -8> * usdt> {
} usdt_e8;

// --- origin -----------------------------------------------------------------

/// @brief Price zero, on either grid.
///
/// A price is a point, not an amount: two prices do not add, their difference
/// is a distance, and a distance moves a price. A midpoint or a VWAP is still
/// expressible - as a reference price plus a weighted mean of distances from
/// it, where the choice of reference cancels - and written that way it cannot
/// overflow, which @c (bid + ask) / 2 on a @c uint32 does.
inline constexpr struct price_zero final
	: mp_units::absolute_point_origin<price> {
} price_zero;

/**
 * @brief Hashes any quantity or quantity point by its numerical value.
 *
 * mp-units supplies no hash, and @c boost::hash cannot find one by ADL: a
 * quantity's unit is a non-type template argument, which contributes no
 * associated namespace. So a map keyed by one names this explicitly.
 */
struct hash {
	template <mp_units::Quantity Q>
	[[nodiscard]] std::size_t operator()(const Q &q) const noexcept {
		return std::hash<typename Q::rep>{}(q.numerical_value_ref_in(Q::unit));
	}

	template <mp_units::QuantityPoint P>
	[[nodiscard]] std::size_t operator()(const P &p) const noexcept {
		return (*this)(p.quantity_ref_from(P::point_origin));
	}
};

/// @brief |@p q| without a branch: @c shift-and-xor on the value, so a
///		   quantity costs exactly what its integer did.
template <mp_units::Quantity Q>
	requires std::signed_integral<typename Q::rep>
[[nodiscard]] constexpr Q abs_of(Q q) noexcept {
	using rep      = typename Q::rep;
	const rep v    = q.numerical_value_in(Q::unit);
	const rep mask = v >> (sizeof(rep) * CHAR_BIT - 1);
	return static_cast<rep>((v ^ mask) - mask) * Q::reference;
}

/// @brief @c -q when @p negate, else @p q, without a branch - the mask is
///        all-ones exactly when negating, and @c (v ^ mask) - mask is then -v.
template <mp_units::Quantity Q>
	requires std::signed_integral<typename Q::rep>
[[nodiscard]] constexpr Q negated_if(Q q, bool negate) noexcept {
	using rep      = typename Q::rep;
	const rep v    = q.numerical_value_in(Q::unit);
	const rep mask = static_cast<rep>(-static_cast<rep>(negate));
	return static_cast<rep>((v ^ mask) - mask) * Q::reference;
}

} // namespace exchange::units
