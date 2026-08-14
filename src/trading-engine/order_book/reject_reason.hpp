#pragma once
#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine {

#define REJECT_REASON_LIST(X)                                                  \
	X(NONE, "not a rejection")                                                 \
	X(NON_POSITIVE_QUANTITY, "order quantity was zero or negative")            \
	X(DUPLICATE_ORDER_ID, "an order with this id is already resting")          \
	X(INSUFFICIENT_LIQUIDITY, "fill-or-kill could not be filled in full")      \
	X(BOOK_AT_CAPACITY, "the book has no room to rest another order")          \
	X(TIME_IN_FORCE, "the remainder was dropped by the order's time-in-force") \
	X(UNKNOWN_ORDER,                                                           \
	  "no resting order with this id — filled, cancelled, or never placed")    \
	X(UNKNOWN_SYMBOL, "no listing for this symbol")                            \
	X(MALFORMED_DECIMAL, "price or quantity was not a well-formed decimal")    \
	X(PRICE_NOT_ON_TICK, "price is not an exact multiple of the tick size")    \
	X(QUANTITY_NOT_ON_LOT,                                                     \
	  "quantity is not an exact multiple of the lot size")                     \
	X(PRICE_OUT_OF_RANGE, "price is more ticks than the engine can represent") \
	X(QUANTITY_OUT_OF_RANGE,                                                   \
	  "quantity is more lots than the engine can represent")                   \
	X(PRICE_OUTSIDE_COLLAR, "price is outside the symbol's price collar")      \
	X(MISSING_STOP_PRICE, "a stop order needs a trigger price")                \
	X(UNEXPECTED_STOP_PRICE, "only a stop order may carry a trigger price")    \
	X(UNSUPPORTED_ORDER_TYPE, "this venue does not match that order type yet") \
	X(RESERVED_ORDER_ID, "order id 0 is the engine's anonymous sentinel")      \
	X(ORDER_ALREADY_FILLED, "the order this cancel names has fully executed")  \
	X(ORDER_ALREADY_CANCELLED,                                                 \
	  "the order this cancel names was already withdrawn")                     \
	X(ORDER_ALREADY_REJECTED,                                                  \
	  "the order this cancel names never entered the book")                    \
	X(RISK_HALTED, "the risk gate is halted and is passing no new orders")     \
	X(RISK_ORDER_QUANTITY, "order quantity exceeds the per-order risk limit")  \
	X(RISK_ORDER_NOTIONAL, "order notional exceeds the per-order risk limit")  \
	X(RISK_PRICE_BAND,                                                         \
	  "price is too far from the last print — fat-finger guard")               \
	X(RISK_POSITION_LIMIT,                                                     \
	  "the order would take the net position past its limit")                  \
	X(RISK_EXPOSURE_LIMIT,                                                     \
	  "the order would take gross exposure past its limit")                    \
	X(RISK_WORKING_ORDERS, "too many orders are already working")              \
	X(RISK_MESSAGE_RATE, "this window's message allowance is spent")

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
 * that is *well-formed and admissible* — the venue could match it and is
 * choosing not to. Keeping them prefixed makes that distinction greppable
 * without a second enum, and @c risk::breach is the bitwise form the gate
 * actually computes with. @see risk::reason_for
 *
 * @c TIME_IN_FORCE is the reason on a CANCELLED, not a REJECTED: an
 * immediate-or-cancel remainder is withdrawn after the order was accepted and
 * possibly executed, so it is a cancellation with a cause, not a refusal.
 *
 * Lives in its own header so @c symbol_spec can name these without including
 * the order lifecycle — static reference data has no business depending on
 * @c order_state.
 */
enum class reject_reason : std::uint8_t {
	EXCHANGE_ENUM_VALUES(REJECT_REASON_LIST)
};

/// @brief The enumerator name of a @c reject_reason, e.g. @c
/// "PRICE_NOT_ON_TICK".
EXCHANGE_ENUM_NAME(reject_reason, to_string, REJECT_REASON_LIST)

/// @brief A short human-readable description of @p reason, for logs and the
///        text a gateway hands back to a client.
EXCHANGE_ENUM_LABEL_ONLY(reject_reason, describe, REJECT_REASON_LIST)

#undef REJECT_REASON_LIST
} // namespace exchange::engine
