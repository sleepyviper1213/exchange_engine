#pragma once
// The gate's scaffolding: the state a batch carries, the band a price is
// measured against, and the trick that turns a rule into a bit.
//
// None of it is anything a caller of `risk_gate` names. It lives here rather
// than in the gate's private section because the gate is a template - every
// private member of a template is in the public header whether it is part of
// the interface or not, so `detail` is the only place that can say it is not.

#include "../breach.hpp"
#include "../fwd.hpp"
#include "risk_management_export.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstdint>
#include <limits>

namespace exchange::risk::detail {

/**
 * @brief @p rule's bit when @p failed, zero otherwise - with no branch.
 *
 * Negating a @c bool gives all-ones or all-zeros, and the AND then either keeps
 * the bit or drops it. This is the whole trick behind @c breach_set, and it is
 * why ten rules cost one branch between them rather than ten.
 */
[[nodiscard]] constexpr breach_bits bit_if(bool failed, breach rule) noexcept {
	return static_cast<breach_bits>(static_cast<unsigned>(rule) &
									-static_cast<unsigned>(failed));
}

/**
 * @brief The prices the fat-finger rule admits, as a floor and a width.
 *
 * Stored that way rather than as a pair of bounds so the check is *one*
 * unsigned compare for a two-sided range: below the floor the subtraction wraps
 * to something enormous and fails the same test that catches a price above the
 * ceiling. The division that produces the width is paid once per print, in
 * @c around, and never on the per-command path.
 *
 * @note Its members are exported, which nothing else in `detail` is, and not
 *       with the AUTOTEST variant: they are defined in screening.cpp but called
 *       from @c risk_gate, and a template is instantiated in the *consumer's*
 *       translation unit. So the symbols cross the library boundary in every
 *       build that links a gate - @c exchange_tool does, with no test in sight
 *       - even though the type is part of nobody's interface. @c probe_table
 *       needs none of this: its only caller is working_ledger.cpp, on this side
 *       of the boundary.
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
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool admits(price_t price) const noexcept;
};

/**
 * @brief Everything hoisted out of the per-command loop, plus what the batch
 *        has provisionally used up so far.
 *
 * Read once in @c risk_gate::open_batch and then only from registers: the
 * clock, the breaker's state, the rate window's headroom and the position are
 * all things the rules would otherwise reload per command, and this thread is
 * the only writer of the last of them. The three @c pending fields are what
 * makes a batch screen against itself - a hundred orders in one call may not
 * each be sized against the position the batch started from.
 */
struct screen_state {
	std::uint64_t now_ns;
	trading_state state;
	std::uint32_t headroom;    ///< messages still allowed this window
	volume_t base_net;         ///< position at batch start
	volume_t base_working_bid; ///< working buys at batch start
	volume_t base_working_ask; ///< working sells at batch start
	volume_t pending_bid  = 0; ///< buys this batch has added
	volume_t pending_ask  = 0; ///< sells this batch has added
	std::uint32_t charged = 0; ///< messages this batch has used
};

} // namespace exchange::risk::detail
