#pragma once
// Order Size Check: the fat-finger rules that need nothing but the order.
//
// Three of them, and they are grouped in one file because they are one question
// asked in three units - lots, and lots times ticks, and the degenerate case
// underneath both. Nothing here reads position, time, or any other order, which
// is why this is the only pre-trade file whose rules are `constexpr` end to end
// and testable with two literals.

#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/detail/screening.hpp"
#include "risk_management/limits.hpp"
#include "orders/types.hpp"

#include <cstdint>

namespace exchange::risk::hooks::pre_trade {

/**
 * @brief Which size rules @p price and @p qty break.
 *
 * @param price In ticks. Only used for the notional; a collar is a separate
 *        rule. @see price_collar.hpp
 * @param qty In lots.
 * @param limits The policy in force.
 * @return @c NON_POSITIVE_QUANTITY, @c ORDER_QUANTITY and @c ORDER_NOTIONAL, in
 *         whatever combination applies. Zero when the order is within its size.
 *
 * @par Why one function for an order and for a level
 * Both a client PLACE and an anonymous ADD are size-checked identically - a
 * seed that is too large is as wrong as an order that is - and before these
 * rules had a home the two paths each spelled the same three lines. @see
 * risk_gate
 *
 * @note The multiplication is widened before it happens, not after. A price
 * near the top of @c price_t times a quantity near the top of @c quantity_t is
 *       about 9.0e18, which fits @c int64 - just; doing it in 32 bits would
 * wrap and turn the largest possible order into a small one.
 *
 * @note @c NON_POSITIVE_QUANTITY is a rule here rather than an assertion
 * because a quantity arrives from outside the process. The book would refuse it
 *       too; the gate refuses it earlier, before it occupies a queue slot, and
 * a client cannot tell which boundary answered. @see reason_for
 */
[[nodiscard]] constexpr breach_bits
size_breaches(price_t price, quantity_t qty,
			  const risk_limits &limits) noexcept {
	const std::int64_t notional =
		static_cast<std::int64_t>(price) * static_cast<std::int64_t>(qty);

	breach_bits mask = 0;
	mask |= bit_if(qty <= 0, breach::NON_POSITIVE_QUANTITY);
	mask |= bit_if(qty > limits.max_order_qty, breach::ORDER_QUANTITY);
	mask |=
		bit_if(notional > limits.max_order_notional, breach::ORDER_NOTIONAL);
	return mask;
}

} // namespace exchange::risk::hooks::pre_trade
