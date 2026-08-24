#pragma once
// Price Collar: the fat-finger band around the mark.
//
// The rule that catches a decimal point in the wrong place - an order to buy at
// 10,000 when the market is 100. Every other pre-trade rule can be satisfied by
// such an order: it is small, it is inside the position limit, its id is fresh.
// Only its price is absurd, and "absurd" is only definable relative to
// something the venue just did.
//
// Moved here from `detail/` when the hooks were given homes: a band is what one
// rule is *about*, so hiding it made the one rule whose state is worth reading
// the one rule a caller could not name.
//
// Its three members stay out of line and exported. The band is derived once per
// print rather than per command, so nothing is gained by inlining it into every
// consumer of a gate - and `risk_gate` is a template, instantiated in the
// consumer's own translation unit, so those definitions would cross the library
// boundary as inline code rather than as the one symbol this way costs.

#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/detail/screening.hpp" // bit_if
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)
#include "orders/types.hpp"

#include <cstdint>
#include <limits>

namespace exchange::risk::hooks::pre_trade {

/**
 * @brief The prices the collar admits, as a floor and a width.
 *
 * Stored that way rather than as a pair of bounds so the check is *one*
 * unsigned compare for a two-sided range: below the floor the subtraction wraps
 * to something enormous and fails the same test that catches a price above the
 * ceiling. The division that produces the width is paid once per print, in
 * @c around, and never on the per-command path.
 */
struct price_band {
	price_t low  = 0;
	price_t span = std::numeric_limits<price_t>::max();

	/**
	 * @brief The band @p half_width_bps wide either side of @p mark.
	 *
	 * @param mark The price to centre on, in ticks. Zero means "not known yet".
	 * @param half_width_bps Half-width in basis points; zero or less disables.
	 * @return An open band - every price within @c span of zero - when there is
	 *         no width to apply or no mark to apply it to.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT static price_band
	around(price_t mark, std::int64_t half_width_bps) noexcept;

	/// @brief The highest price admitted, in ticks.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT price_t high() const noexcept;

	/// @brief Whether @p price is inside the band. One unsigned compare.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	admits(price_t price) const noexcept;
};

/**
 * @brief Whether @p price is outside @p band, as a bit.
 *
 * @return @c PRICE_BAND, or zero.
 *
 * @note The band is a *cached* rule: it is re-derived on every print by
 *       @c risk_gate::set_reference_price rather than recomputed here, because
 *       the input that moves is the mark and not the order. So this is a
 * compare against a value already in the gate's cache line, and the collar
 * costs the same as the size rules despite being the only one defined by
 *       reference to the market.
 */
[[nodiscard]] inline breach_bits collar_breach(const price_band &band,
											   price_t price) noexcept {
	return bit_if(!band.admits(price), breach::PRICE_BAND);
}

} // namespace exchange::risk::hooks::pre_trade