#pragma once
// A withdrawal on its way *out*, named the way the order was sent.
//
// Carries the `client_order_id` rather than the venue's own identifier, for the
// reason spelled out below: our id exists from the moment we mint it, the
// venue's only from the moment it acks. @see outbound_order.hpp

#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <string>

namespace exchange::venue {

/// @brief An order to withdraw, named the way it was sent. @see outbound_order
struct outbound_cancel {
	std::string symbol{};

	/// @brief The @c client_order_id of the order to cancel. Cancelling by our
	///        own id rather than the venue's means a cancel can be issued
	///        before the placement's ack has come back, which is exactly when
	///        a strategy most wants to.
	std::string client_order_id{};
};

} // namespace exchange::venue
