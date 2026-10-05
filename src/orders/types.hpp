#pragma once

#include "side.hpp"  // IWYU pragma: export
#include "units.hpp" // IWYU pragma: export

#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace exchange {

/**
 * @brief A price level in **ticks** - never a scaled decimal.
 *
 * The engine matches on the integer grid @c symbol_spec defines, so a value of
 * this type is a tick count and nothing else: @c "153.45" on a listing with a
 * 0.05 tick is 3069 here, not 15345. 32 bits is the whole tick domain of any
 * listing that could be traded - a $60,000 instrument on a $0.01 tick spans 6
 * million ticks, and this holds 4.29 billion - and it is what keeps a resting
 * order two to a cache line.
 *
 * A point, not an amount: @c price + @c price does not compile, @c price -
 * @c price is a @c tick_span_t, and a distance moves a price. @see at_tick for
 * spelling one, @c exact_mid for the midpoint that sum would have been for.
 *
 * @warning A point difference keeps the point's @c uint32 representation, so
 *          @c bid @c - @c ask wraps rather than going negative. Use
 *          @c ticks_between where the order of the two is not known.
 * @warning A **scaled** price is @c scaled_price_t, in a unit with no relation
 *          to @c units::tick, so the two do not mix at all.
 *          @c symbol_spec::price_from_scaled is the only sanctioned crossing,
 *          and it range-checks rather than truncates.
 */
using price_t =
	mp_units::quantity_point<units::tick, units::price_zero, std::uint32_t>;

/// @brief A signed distance between two prices, in ticks. 64-bit so the
///        difference of any two @c price_t is representable either way round.
using price_delta_t = mp_units::quantity<units::tick, std::int64_t>;

/// @brief An unsigned distance in ticks at the price's own width - what
///        @c price @c - @c price yields, and the offset a quote or a band is
///        set at. Wraps below zero exactly as the price it came from would,
///        which @c price_band relies on. @see price_delta_t for a signed one.
using tick_span_t = mp_units::quantity<units::tick, price_t::rep>;

/**
 * @brief One order's quantity in **lots**, on the grid @c symbol_spec defines.
 *
 * Signed because the validation boundary is stated as @c qty <= 0 and because
 * an order's remaining quantity is arithmetic that wants a sign during
 * intermediate steps - not because a resting order may be negative. It may not.
 *
 * @warning This is a *per-order* quantity, bounded by
 *          @c symbol_spec::quantity_from_scaled's range check. Anything that
 *          sums quantities across orders or levels must use @c volume_t: a
 *          level's aggregate, or the liquidity a fill-or-kill adds up, can
 *          exceed one order's range without any single order doing so. The
 *          conversion into @c volume_t is implicit; the one back out is a
 *          @c quantity_cast, because it can narrow. @see units::order_size
 */
using quantity_t =
	mp_units::quantity<units::order_size[units::lot], std::int32_t>;

/**
 * @brief Aggregate quantity - a sum of @c quantity_t across orders or levels.
 *
 * Exists so narrowing an order's quantity cannot silently narrow the totals
 * built from it. A level holding 100,000 orders of a billion lots each is not a
 * realistic book, but the addition that would wrap is one line in the matching
 * loop and the loop is the wrong place to find out. 64 bits removes the
 * question rather than answering it.
 */
using volume_t = mp_units::quantity<units::size[units::lot], std::int64_t>;

/**
 * @brief Price times size, in tick-lots.
 *
 * Not a currency amount - converting to one needs the listing's scales, which
 * live on @c symbol_spec - but exactly proportional to one, so two notionals of
 * the same listing compare with no conversion. Build one with @c notional_of,
 * never with a bare @c *: a @c uint32 price times an @c int32 size has a
 * @c uint32 product under the usual arithmetic conversions, and mp-units passes
 * the representation through.
 */
using notional_t = mp_units::quantity<units::tick * units::lot, std::int64_t>;

/**
 * @brief An amount of money, counted in 10^-8 USDT.
 *
 * What a @c notional_t is worth once a listing says what its tick-lot is: the
 * unit two listings can be added in, where their tick-lots cannot. One SOLUSDT
 * tick-lot is 10^-5 USDT and one BTCUSDT tick-lot is 10^-7, so a sum of the two
 * as @c notional_t is off by a factor of a hundred and compiles; as @c usdt_t
 * it is exact. @c symbol_spec::usdt_from is the crossing.
 *
 * Exact integers, never a @c double: an amount that went through a rounding is
 * one nobody can reconcile. 64 bits at 10^-8 holds 9.2 * 10^10 USDT.
 */
using usdt_t =
	mp_units::quantity<units::currency[units::usdt_e8], std::int64_t>;

/// @brief Decimal places in a @c usdt_t: the scale its bare count is read on.
inline constexpr int USDT_SCALE = 8;

/// @brief @p amount as a bare count of 10^-8 USDT, for the decimal formatter.
[[nodiscard]] constexpr std::int64_t usdt_e8_of(usdt_t amount) noexcept {
	return amount.numerical_value_in(units::usdt_e8);
}

/// @brief The price @p ticks ticks above zero - how a literal price is spelled.
[[nodiscard]] constexpr price_t at_tick(std::uint32_t ticks) noexcept {
	return units::price_zero + ticks * units::tick;
}

/// @brief Price zero: never a valid order price - @c symbol_spec::collar_low
///        is at least one tick - which is what lets it mean "absent" in a
///        field that cannot afford an @c optional's extra word.
inline constexpr price_t NO_PRICE = at_tick(0);

/// @brief @p price as a bare tick count, for the boundaries that need one:
///        an array index, a wire field, a SIMD lane.
[[nodiscard]] constexpr std::uint32_t ticks_of(price_t price) noexcept {
	return price.quantity_from_zero().numerical_value_in(units::tick);
}

/// @brief @p qty as a bare lot count. @see ticks_of
[[nodiscard]] constexpr std::int32_t lots_of(quantity_t qty) noexcept {
	return qty.numerical_value_in(units::lot);
}

/// @copydoc lots_of(quantity_t)
[[nodiscard]] constexpr std::int64_t lots_of(volume_t volume) noexcept {
	return volume.numerical_value_in(units::lot);
}

/**
 * @brief @p volume as one order's quantity - the one narrowing crossing.
 *
 * @pre @p volume fits an order's range. Every caller has already bounded it by
 *      a single order's own remaining quantity, which is why this asserts
 *      rather than rejects: a failure is a matching bug, not bad input.
 */
[[nodiscard]] constexpr quantity_t order_quantity(volume_t volume) noexcept {
	assert(volume >= quantity_t::min() && volume <= quantity_t::max() &&
		   "an aggregate does not fit one order's quantity");
	return mp_units::value_cast<quantity_t::rep>(
		mp_units::quantity_cast<units::order_size>(volume));
}



/// @brief @p to minus @p from, signed: negative when @p to is the lower price.
[[nodiscard]] constexpr price_delta_t ticks_between(price_t from,
													price_t to) noexcept {
	return mp_units::value_cast<std::int64_t>(to) -
		   mp_units::value_cast<std::int64_t>(from);
}

/// @brief @p price times @p volume, widened before the multiply.
[[nodiscard]] constexpr notional_t notional_of(price_t price,
											   volume_t volume) noexcept {
	return mp_units::value_cast<std::int64_t>(price.quantity_from_zero()) *
		   volume;
}
// --- the feed's grid --------------------------------------------------------

/**
 * @brief A price as a venue states it: the decimal text times 10^price_scale.
 *
 * @c "153.45" is 15345 at scale 2 and 15'345'000'000 at scale 8. The same
 * price as a @c price_t is that divided by the listing's tick - around 10^6
 * where this is around 10^12 - and the two used to be one typedef, which hid
 * the difference. They are separate units now, @c units::scaled_price and
 * @c units::tick, with no conversion between them, so comparing, subtracting
 * or assigning across the two grids does not compile.
 * @c symbol_spec::price_from_scaled is the crossing, because only the spec
 * knows the tick and can refuse a value that is off it.
 *
 * Signed because @c core::scaled::parse_fixed_point is, and casting a
 * negative parse into an unsigned type turned a bad number into a huge
 * valid-looking price. 64-bit because the scale is a run-time choice
 * (@c --price-decimals), and at scale 8 a five-figure price needs ~40 bits.
 */
using scaled_price_t =
	mp_units::quantity_point<units::scaled_price, units::price_zero,
							 std::int64_t>;

/// @brief A distance on the feed's grid: a tick size, a spread, an impact.
///        What @c scaled_price_t @c - @c scaled_price_t yields.
using scaled_price_delta_t =
	mp_units::quantity<units::scaled_price, std::int64_t>;

/**
 * @brief A size as a venue states it, scaled by 10^qty_scale.
 *
 * An aggregate - a level's resting size, or a print's - so it is a @c size and
 * not an @c order_size, and it has no conversion to @c volume_t for the same
 * reason @c scaled_price_t has none to @c price_t. Signed because an L2 diff
 * spells a level's removal as a non-positive size.
 */
using scaled_qty_t =
	mp_units::quantity<units::size[units::scaled_size], std::int64_t>;

/// @brief The scaled price @p scaled above zero - how a literal one is spelled.
[[nodiscard]] constexpr scaled_price_t at_scaled(std::int64_t scaled) noexcept {
	return units::price_zero + scaled * units::scaled_price;
}

/// @brief @p price as a bare scaled integer, for the boundaries that need one:
///        the decimal formatter, a wire field, a SIMD lane. @see ticks_of
[[nodiscard]] constexpr std::int64_t scaled_of(scaled_price_t price) noexcept {
	return price.quantity_from_zero().numerical_value_in(units::scaled_price);
}

/// @copydoc scaled_of(scaled_price_t)
[[nodiscard]] constexpr std::int64_t
scaled_of(scaled_price_delta_t delta) noexcept {
	return delta.numerical_value_in(units::scaled_price);
}

/// @copydoc scaled_of(scaled_price_t)
[[nodiscard]] constexpr std::int64_t scaled_of(scaled_qty_t qty) noexcept {
	return qty.numerical_value_in(units::scaled_size);
}

/// @brief Stable identifier for a client order. Stays 64-bit: it is assigned
///        outside the engine and carries no density contract, which is exactly
///        why cancel-by-id still goes through a hash map. @see order_book
using order_id_t = std::uint64_t;

/**
 * @brief Where a command sat in one partition's applied stream, stamped on
 *        every record that command produced.
 *
 * Not assigned by whoever built the command: it is the command's *ordinal*,
 * counted by the partition as it applies the batch, and therefore the index of
 * that same command in that partition's journal. Which is the point - a
 * producer-assigned number could disagree with the log, and a gap or a repeat
 * would then be indistinguishable from a command that never made it. Counting
 * on the way out makes both impossible by construction, and makes replay
 * verifiable: replaying a journal re-derives exactly the numbers the live run
 * stamped, so the two streams compare record for record.
 *
 * Monotonic and gapless within a partition, and meaningless across two - they
 * are independent streams over disjoint listings, and a total order between
 * them would be a synchronisation point on the one path that has none.
 *
 * @note Zero is "unsequenced": a record built outside a partition, such as a
 *       venue report crossing @c session::venue_bridge, or one produced by an
 *       @c order_book driven directly by a test or a benchmark.
 * @note @c engine_ and not a bare @c sequence_t, because
 *       @c market_data::sequence_t already means something else entirely - the
 *       update id a *venue* stamps on a depth diff, which arrives from outside
 *       and is checked for gaps rather than issued. The two would be an
 *       ambiguity in any scope that opened both namespaces, and a far worse
 *       confusion in prose. Same reason @c l2_book and @c order_book are named
 *       apart: different concepts do not share a name here.
 */
using engine_sequence_t = std::uint64_t;

/**
 * @brief A listing's execution number: 1 for its first trade, and up from
 *        there.
 *
 * Per listing rather than per venue, because a tape is per listing - a consumer
 * of one instrument's prints wants them numbered 1, 2, 3, and a venue-wide
 * counter would hand it an arbitrary subsequence with no way to tell a gap from
 * a message it dropped. @c (symbol_id, trade_id) is the venue-unique name, and
 * the symbol is already reattached by the time a trade leaves the partition.
 * @see event::engine_event
 *
 * Dense and gapless by construction: @c order_book assigns it where the trade
 * is created, so there is no path that prints an execution without numbering
 * it.
 *
 * @note Zero means unassigned, which is what a trade built by hand carries.
 */
using trade_id_t = std::uint64_t;

/// @brief A point in time, in nanoseconds since the Unix epoch. Zero means "not
///        stamped". The clock is read at the venue boundary and nowhere on the
///        matching path. @see engine::trade::timestamp
using timestamp_t = std::uint64_t;

/// @brief Dense identifier for a listing, assigned by the reference-data
/// source.
///        Dense because it indexes the book manager's per-symbol arrays.
using symbol_id_t = std::uint32_t;

/**
 * @brief Who an order belongs to - the participant the venue will bill and
 *        report to.
 *
 * Assigned at the gateway when a session authenticates, so it is trusted by the
 * time an order carries it and never comes off the wire. 32 bits because it
 * names a member of the venue rather than a client order: an order_book has
 * thousands of participants, not billions, and the narrower type is what keeps
 * an @c order_record inside its size budget.
 *
 * Zero means unattributed, which is what anonymous seeded liquidity carries.
 * Self-trade prevention is the reason this exists - two orders may not cross if
 * they name the same account - but nothing enforces that yet.
 */
using account_id_t = std::uint32_t;

// The strong types are the integers they replaced, byte for byte: the journal
// bit_casts them and the resting-order layout budgets them.
static_assert(sizeof(price_t) == sizeof(std::uint32_t));
static_assert(std::is_same_v<decltype(price_t{} - price_t{}), tick_span_t>);
static_assert(sizeof(quantity_t) == sizeof(std::int32_t));
static_assert(sizeof(volume_t) == sizeof(std::int64_t));
static_assert(std::is_trivially_copyable_v<price_t> &&
			  std::is_trivially_copyable_v<quantity_t> &&
			  std::is_trivially_copyable_v<volume_t>);
static_assert(std::is_unsigned_v<price_t::rep>, "Price must be unsigned");
static_assert(std::is_signed_v<quantity_t::rep>,
			  "Quantity must be signed: the validation boundary is qty <= 0");
static_assert(
	sizeof(volume_t) >= 2 * sizeof(quantity_t),
	"volume_t must be wide enough that summing quantities cannot wrap");
static_assert(sizeof(usdt_t) == sizeof(std::int64_t) &&
			  std::is_trivially_copyable_v<usdt_t>);
static_assert(!std::is_constructible_v<usdt_t, notional_t> &&
				  !std::is_constructible_v<notional_t, usdt_t> &&
				  !std::equality_comparable_with<usdt_t, notional_t>,
			  "Tick-lots become money through symbol_spec only");
static_assert(1 * units::usdt == usdt_t{100'000'000 * units::usdt_e8});
static_assert(std::is_convertible_v<quantity_t, volume_t> &&
				  !std::is_convertible_v<volume_t, quantity_t>,
			  "Widening into volume_t is implicit; narrowing out of it is not");

// The feed's grid is the integer it replaced too - l2_book reads its cells as
// a flat int64 array - and shares nothing with the matching grid.
static_assert(sizeof(scaled_price_t) == sizeof(std::int64_t) &&
			  sizeof(scaled_qty_t) == sizeof(std::int64_t));
static_assert(std::is_trivially_copyable_v<scaled_price_t> &&
			  std::is_trivially_copyable_v<scaled_qty_t>);
static_assert(std::is_same_v<decltype(scaled_price_t{} - scaled_price_t{}),
							 scaled_price_delta_t>);
static_assert(!std::is_constructible_v<price_t, scaled_price_t> &&
				  !std::is_constructible_v<scaled_price_t, price_t> &&
				  !std::equality_comparable_with<price_t, scaled_price_t>,
			  "A scaled price crosses into ticks through symbol_spec only");
static_assert(!std::is_constructible_v<volume_t, scaled_qty_t> &&
				  !std::is_constructible_v<scaled_qty_t, volume_t> &&
				  !std::equality_comparable_with<volume_t, scaled_qty_t>,
			  "A scaled size crosses into lots through symbol_spec only");

} // namespace exchange
