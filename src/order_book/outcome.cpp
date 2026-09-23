#include "outcome.hpp"

#include "order_status.hpp" // IWYU pragma: export

namespace exchange::engine {

order_outcome order_outcome::accepted(order_id_t id,
									  quantity_t quantity) noexcept {
	return {.id        = id,
			.type      = OutcomeType::ACCEPTED,
			.reason    = reject_reason::NONE,
			.status    = OrderStatus::LIVE,
			.traded    = 0,
			.remaining = quantity,
			.sequence  = 0,
			.trade_id  = 0};
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
			.remaining = quantity,
			.sequence  = 0,
			.trade_id  = 0};
}

order_outcome order_outcome::fill(order_id_t id, const order_state &state,
								  trade_id_t trade) noexcept {
	return {.id        = id,
			.type      = OutcomeType::FILL,
			.reason    = reject_reason::NONE,
			.status    = state.status(),
			.traded    = state.traded(),
			.remaining = state.remaining(),
			.sequence  = 0,
			.trade_id  = trade};
}

order_outcome order_outcome::cancelled(order_id_t id, const order_state &state,
									   reject_reason reason) noexcept {
	return {.id        = id,
			.type      = OutcomeType::CANCELLED,
			.reason    = reason,
			.status    = state.status(),
			.traded    = state.traded(),
			.remaining = state.remaining(),
			.sequence  = 0,
			.trade_id  = 0};
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
			.remaining = 0,
			.sequence  = 0,
			.trade_id  = 0};
}

order_outcome order_outcome::modified(order_id_t id,
									  const order_state &state) noexcept {
	// The state *after* the amendment and before anything it causes, which is
	// the same reading every other record here has. An amendment that moved the
	// price crosses next, and those fills report themselves.
	return {.id        = id,
			.type      = OutcomeType::MODIFIED,
			.reason    = reject_reason::NONE,
			.status    = state.status(),
			.traded    = state.traded(),
			.remaining = state.remaining(),
			.sequence  = 0,
			.trade_id  = 0};
}

order_outcome order_outcome::modify_rejected(order_id_t id,
											 reject_reason reason) noexcept {
	// Same silence as cancel_rejected: an amendment the book declined leaves
	// the order exactly as it was, and the book does not restate a state it did
	// not change. Where the refusal is UNKNOWN_ORDER there is no state to
	// restate at all.
	return {.id        = id,
			.type      = OutcomeType::MODIFY_REJECTED,
			.reason    = reason,
			.status    = OrderStatus::NEW,
			.traded    = 0,
			.remaining = 0,
			.sequence  = 0,
			.trade_id  = 0};
}

} // namespace exchange::engine
