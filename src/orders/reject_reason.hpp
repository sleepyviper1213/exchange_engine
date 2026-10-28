#pragma once
#include "core/util/enum_format.hpp"
#include "fwd.hpp"

#include <cstdint>
#include <string_view>

namespace exchange::engine {

using core::util::format_as;

/**
 * @brief Why an order was rejected, or a cancel request declined.
 *
 * One vocabulary for three boundaries, because a client cannot tell them apart
 * and should not have to: the reasons down to @c UNKNOWN_ORDER come from the
 * book, which knows about resting orders, liquidity and its own capacity; the
 * ones after that come from the validation stage in @c symbol/, which knows
 * about the listing's decimal conventions; and the @c RISK_ prefixed tail comes
 * from @c risk/, which knows about position, exposure and rate. All three
 * arrive on the same outcome stream.
 *
 * The @c RISK_ ones are the only refusals a client can receive for an order
 * that is *well-formed and admissible* - the venue could match it and is
 * choosing not to. Keeping them prefixed makes that distinction greppable
 * without a second enum, and @c risk::breach is the bitwise form the gate
 * actually computes with. @see risk::reason_for
 *
 * @c TIME_IN_FORCE is the reason on a CANCELLED, not a REJECTED: an
 * immediate-or-cancel remainder is withdrawn after the order was accepted and
 * possibly executed, so it is a cancellation with a cause, not a refusal.
 *
 * Lives in its own header so @c symbol_spec can name these without including
 * the order lifecycle - static reference data has no business depending on
 * @c order_state.
 */
enum class reject_reason : std::uint8_t {
	NONE,
	NON_POSITIVE_QUANTITY,
	DUPLICATE_ORDER_ID,
	INSUFFICIENT_LIQUIDITY,
	BOOK_AT_CAPACITY,
	TIME_IN_FORCE,
	UNKNOWN_ORDER,
	UNKNOWN_SYMBOL,
	MALFORMED_DECIMAL,
	PRICE_NOT_ON_TICK,
	QUANTITY_NOT_ON_LOT,
	PRICE_OUT_OF_RANGE,
	QUANTITY_OUT_OF_RANGE,
	PRICE_OUTSIDE_COLLAR,
	MISSING_STOP_PRICE,
	UNEXPECTED_STOP_PRICE,
	UNSUPPORTED_ORDER_TYPE,
	UNSUPPORTED_TIME_IN_FORCE,
	RESERVED_ORDER_ID,
	ORDER_ALREADY_FILLED,
	ORDER_ALREADY_CANCELLED,
	ORDER_ALREADY_REJECTED,
	RISK_HALTED,
	RISK_ORDER_QUANTITY,
	RISK_ORDER_NOTIONAL,
	RISK_PRICE_BAND,
	RISK_POSITION_LIMIT,
	RISK_EXPOSURE_LIMIT,
	RISK_WORKING_ORDERS,
	RISK_MESSAGE_RATE,
	VENUE_REJECTED,
};

/// @brief A short human-readable description of @p reason, for logs and the
///        text a gateway hands back to a client. The enum itself prints as its
///        enumerator name, e.g. @c "PRICE_NOT_ON_TICK".
[[nodiscard]] constexpr std::string_view
describe(reject_reason reason) noexcept {
	switch (reason) {
		using enum reject_reason;
	case NONE: return "not a rejection";
	case NON_POSITIVE_QUANTITY: return "order quantity was zero or negative";
	case DUPLICATE_ORDER_ID: return "an order with this id is already resting";
	case INSUFFICIENT_LIQUIDITY:
		return "fill-or-kill could not be filled in full";
	case BOOK_AT_CAPACITY: return "the book has no room to rest another order";
	case TIME_IN_FORCE:
		return "the remainder was dropped by the order's time-in-force";
	case UNKNOWN_ORDER:
		return "no resting order with this id - filled, cancelled, or never "
			   "placed";
	case UNKNOWN_SYMBOL: return "no listing for this symbol";
	case MALFORMED_DECIMAL:
		return "price or quantity was not a well-formed decimal";
	case PRICE_NOT_ON_TICK:
		return "price is not an exact multiple of the tick size";
	case QUANTITY_NOT_ON_LOT:
		return "quantity is not an exact multiple of the lot size";
	case PRICE_OUT_OF_RANGE:
		return "price is more ticks than the engine can represent";
	case QUANTITY_OUT_OF_RANGE:
		return "quantity is more lots than the engine can represent";
	case PRICE_OUTSIDE_COLLAR:
		return "price is outside the symbol's price collar";
	case MISSING_STOP_PRICE: return "a stop order needs a trigger price";
	case UNEXPECTED_STOP_PRICE:
		return "only a stop order may carry a trigger price";
	case UNSUPPORTED_ORDER_TYPE:
		return "this venue does not match that order type yet";
	case UNSUPPORTED_TIME_IN_FORCE:
		return "this venue does not honour that time-in-force yet";
	case RESERVED_ORDER_ID:
		return "order id 0 is the engine's anonymous sentinel";
	case ORDER_ALREADY_FILLED:
		return "the order this cancel names has fully executed";
	case ORDER_ALREADY_CANCELLED:
		return "the order this cancel names was already withdrawn";
	case ORDER_ALREADY_REJECTED:
		return "the order this cancel names never entered the book";
	case RISK_HALTED:
		return "the risk gate is halted and is passing no new orders";
	case RISK_ORDER_QUANTITY:
		return "order quantity exceeds the per-order risk limit";
	case RISK_ORDER_NOTIONAL:
		return "order notional exceeds the per-order risk limit";
	case RISK_PRICE_BAND:
		return "price is too far from the last print - fat-finger guard";
	case RISK_POSITION_LIMIT:
		return "the order would take the net position past its limit";
	case RISK_EXPOSURE_LIMIT:
		return "the order would take gross exposure past its limit";
	case RISK_WORKING_ORDERS: return "too many orders are already working";
	case RISK_MESSAGE_RATE: return "this window's message allowance is spent";
	case VENUE_REJECTED: return "an external venue declined the order";
	}
	return {};
}

} // namespace exchange::engine