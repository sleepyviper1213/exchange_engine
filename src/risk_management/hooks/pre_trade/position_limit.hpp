#pragma once
// Position Limit: net inventory and gross exposure, measured against what the
// book would do if every working order filled.
//
// The rule that separates a risk system from a size filter. An order that is
// individually tiny is still refused if the position it would leave behind is
// too large, and "would leave behind" is the load-bearing phrase: an order that
// has been sent is exposure whether or not it has filled yet, so the projection
// counts the working orders too. Checking the *current* position instead would
// let a strategy build any position it liked in one batch of small orders.

#include "core/util/branchless.hpp"                   // abs_of
#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/detail/screening.hpp" // bit_if, screen_state
#include "risk_management/hooks/fwd.hpp"
#include "risk_management/limits.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

namespace exchange::risk::hooks::pre_trade {

/**
 * @brief Which position rules @p o would break, given the batch's @p state.
 *
 * @param o The order being screened. Its side and quantity are the projection's
 *        only inputs; its price is not - exposure is valued at @p mark, because
 *        an aggressive buy fills at the market's price and not at its own
 * limit.
 * @param state Everything hoisted out of the per-command loop, including what
 *        earlier commands *in this batch* have provisionally added.
 *        @see screen_state
 * @param limits The policy in force.
 * @param mark The reference price, in ticks. Zero values exposure at zero,
 * which is the honest answer before the first print rather than a refusal.
 * @return @c POSITION_LIMIT and @c EXPOSURE_LIMIT, in whatever combination
 *         applies.
 *
 * @par The two projections, and why gross is not a sum
 * Net is one-sided: this order against the current position, so a sell against
 * a long reduces and is admitted where a buy would be refused.
 *
 * Gross takes the *worse* of two hypotheticals - every resting buy fills, or
 * every resting sell does - rather than adding the two sides together. Summing
 * them would double-count a two-sided quote: a market maker showing 10 up and
 * 10 down cannot end up 20 long, it ends up 10 either way, and a limit that
 * pretended otherwise would halve every quoting strategy's allowance for
 * nothing.
 *
 * @note Branchless throughout - the side selects with a conditional expression
 * on a value already in a register, and @c abs_of is a shift and two arithmetic
 * ops rather than a compare. There is no early return, because a refused order
 * must cost what an accepted one costs. @see risk_gate
 */
[[nodiscard]] inline breach_bits
exposure_breaches(const engine::orders::order &o, const screen_state &state,
				  const risk_limits &limits, price_t mark) noexcept {
	using core::util::abs_of;

	const bool buying = o.side == side_t::bid;
	const auto lots   = static_cast<volume_t>(o.qty);

	// If this order and everything already working on each side filled.
	const volume_t bid_after =
		state.base_working_bid + state.pending_bid + (buying ? lots : 0);
	const volume_t ask_after =
		state.base_working_ask + state.pending_ask + (buying ? 0 : lots);
	const volume_t if_bids_fill = abs_of(state.base_net + bid_after);
	const volume_t if_asks_fill = abs_of(state.base_net - ask_after);
	const volume_t gross =
		if_bids_fill > if_asks_fill ? if_bids_fill : if_asks_fill;

	// This order alone, against the net position.
	const volume_t signed_lots = buying ? lots : -lots;
	const volume_t net_after   = abs_of(state.base_net + signed_lots);

	breach_bits mask = 0;
	mask |=
		bit_if(net_after > limits.max_position_lots, breach::POSITION_LIMIT);
	mask |= bit_if(gross * static_cast<volume_t>(mark) >
					   limits.max_exposure_notional,
				   breach::EXPOSURE_LIMIT);
	return mask;
}

} // namespace exchange::risk::hooks::pre_trade
