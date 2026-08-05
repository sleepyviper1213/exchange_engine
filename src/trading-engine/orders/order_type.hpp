#pragma once

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

namespace exchange::engine::orders {

#define ORDER_TYPE_LIST(X)                                                     \
	X(MARKET, "take whatever the book offers; no price limit")                 \
	X(LIMIT, "trade only at the order's price or better")                      \
	X(STOP, "dormant until the market trades through the trigger price")

/**
 * @brief What price an order is willing to trade at.
 *
 * The *price* half of an order's instructions, and orthogonal to the *duration*
 * half in @c time_in_force_instruction: "limit" and "immediate or cancel" are
 * answers to different questions, and an order carries one of each. Splitting
 * them is what lets a marketable-limit IOC be expressed without a combinatorial
 * enumerator per pairing.
 *
 * @see https://www.interactivebrokers.com/en/trading/ordertypes.php
 *
 * @note @c STOP is declared but not matched: nothing in @c order_book watches a
 *       trigger price yet, so one is refused with @c UNSUPPORTED_ORDER_TYPE
 *       rather than rested like a limit — a stop that becomes live the instant
 *       it arrives is the opposite of what was asked for, and doing it silently
 *       is worse than declining. The enumerator and @c order::stop_price exist
 *       so the trigger machinery has somewhere to grow into.
 */
enum class order_type : std::uint8_t { EXCHANGE_ENUM_VALUES(ORDER_TYPE_LIST) };

/// @brief The enumerator name of an @c order_type, e.g. @c "LIMIT" (empty view
///        if out of range).
EXCHANGE_ENUM_NAME(order_type, to_string, ORDER_TYPE_LIST)

} // namespace exchange::engine::orders
