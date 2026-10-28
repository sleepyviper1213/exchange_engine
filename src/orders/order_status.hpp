#pragma once
// Where an order sits in its lifecycle.
//
// Split from order_state.hpp because the status is what everything downstream
// reads - outcomes, the order manager's records, a client report - while the
// quantity machine that derives it is `order_state`'s own business.

#include "core/util/enum_format.hpp"
#include "fwd.hpp"

namespace exchange::engine {

using core::util::format_as;

/// @brief Where an order sits in its lifecycle.
///
/// NEW and REJECTED belong to the validation boundary rather than to the book:
/// an order is NEW only until @c place_order decides, and a REJECTED one never
/// gets an @c order_state at all. Both are still reportable on the outcome
/// stream, which is why they are enumerated here and not only in @c
/// order_state.
enum class OrderStatus : std::uint8_t {
	NEW,              ///< validated but not yet accepted by the book
	LIVE,             ///< accepted, resting, nothing executed yet
	PARTIALLY_FILLED, ///< some quantity executed, some still resting
	FILLED,           ///< fully executed; terminal
	CANCELLED,        ///< withdrawn with quantity still unexecuted; terminal
	REJECTED,         ///< never entered the book; terminal
};

/// @brief LIVE or PARTIALLY_FILLED - the order can still fill or be cancelled.
[[nodiscard]] constexpr bool is_active(OrderStatus s) noexcept {
	return s == OrderStatus::LIVE || s == OrderStatus::PARTIALLY_FILLED;
}

/// @brief FILLED, CANCELLED or REJECTED - no transition may leave this state.
[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
	return s == OrderStatus::FILLED || s == OrderStatus::CANCELLED ||
		   s == OrderStatus::REJECTED;
}

} // namespace exchange::engine
