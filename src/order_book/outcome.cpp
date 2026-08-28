#include "outcome.hpp"
#include "order_status.hpp"          // IWYU pragma: export

namespace exchange::engine {

order_outcome order_outcome::accepted(order_id_t id,
									quantity_t quantity) noexcept {
	return {.id        = id,
			.type      = OutcomeType::ACCEPTED,
			.reason    = reject_reason::NONE,
			.status    = OrderStatus::LIVE,
			.traded    = 0,
			.remaining = quantity};
}

order_outcome order_outcome::rejected(order_id_t id, reject_reason reason,
									quantity_t quantity) noexcept {
	// A rejected order executed nothing, so its whole quantity is unexecuted.
	// Reported rather than zeroed so a client can reconcile against what it
	// sent without holding on to the original request.
	return {.id        = id,
			.type      = OutcomeType::REJECTED,
			.reason    = reason,
			.status    = OrderStatus::REJECTED,
			.traded    = 0,
			.remaining = quantity};
}

order_outcome order_outcome::fill(order_id_t id,
								const order_state &state) noexcept {
	return {.id        = id,
			.type      = OutcomeType::FILL,
			.reason    = reject_reason::NONE,
			.status    = state.status(),
			.traded    = state.traded(),
			.remaining = state.remaining()};
}

order_outcome order_outcome::cancelled(order_id_t id, const order_state &state,
									 reject_reason reason) noexcept {
	return {.id        = id,
			.type      = OutcomeType::CANCELLED,
			.reason    = reason,
			.status    = state.status(),
			.traded    = state.traded(),
			.remaining = state.remaining()};
}

order_outcome order_outcome::cancel_rejected(order_id_t id,
										   reject_reason reason) noexcept {
	// No record of the order survives, so there is nothing truthful to put in
	// status/traded/remaining. @see the note on order_outcome.
	return {.id        = id,
			.type      = OutcomeType::CANCEL_REJECTED,
			.reason    = reason,
			.status    = OrderStatus::NEW,
			.traded    = 0,
			.remaining = 0};
}

} // namespace exchange::engine
