#pragma once
// An amendment on its way *out*, for a venue that amends by cancel-and-replace.
//
// Binance spot cannot move a working order's price: its only in-place
// amendment reduces quantity. A reprice is therefore one atomic request that
// withdraws one venue order and places another, and the two need different
// client ids - the venue refuses an id still attached to a working order.
// @see session::venue_legs, which is what chooses them.

#include "venue/outbound_order.hpp"
#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <string>

namespace exchange::venue {

/// @brief Withdraw one venue order and place @c replacement in its stead.
struct outbound_replace {
	/// @brief The new order, under its own client id. Its size is what should
	///        rest, not the engine order's lifetime quantity - the venue counts
	///        a replacement's executions from zero.
	outbound_order replacement{};

	/// @brief The client id of the venue order being withdrawn.
	std::string cancel_client_order_id{};
};

} // namespace exchange::venue
