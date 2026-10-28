#pragma once

#include "core/util/enum_format.hpp"
#include "fwd.hpp"

namespace exchange::engine::orders {

using core::util::format_as;

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
 *       rather than rested like a limit - a stop that becomes live the instant
 *       it arrives is the opposite of what was asked for, and doing it silently
 *       is worse than declining. The enumerator and @c order::stop_price exist
 *       so the trigger machinery has somewhere to grow into.
 */
enum class order_type : std::uint8_t {
	MARKET, ///< take whatever the book offers; no price limit
	LIMIT,  ///< trade only at the order's price or better
	STOP,   ///< dormant until the market trades through the trigger price
};

} // namespace exchange::engine::orders
