#pragma once
#include "core/types.hpp"
#include "core/util/enum_string.hpp"
#include "fwd.hpp"
#include "order_state.hpp"
#include "reject_reason.hpp"

#include <cstdint>
#include <type_traits>

namespace exchange::engine {

#define OUTCOME_TYPE_LIST(X)                                                   \
	X(ACCEPTED, "the book took the order; it is now LIVE")                     \
	X(REJECTED, "the order never entered the book")                            \
	X(FILL, "quantity executed against this order")                            \
	X(CANCELLED, "the unexecuted remainder was withdrawn")                     \
	X(CANCEL_REJECTED, "a cancel request the book could not apply")

/**
 * @brief What happened to an order.
 *
 * This is the transition, not the resulting state — @c OrderOutcome carries
 * both, because they are not the same question and a client needs each. A FILL
 * leaves the order PARTIALLY_FILLED or FILLED; only the outcome type says an
 * execution is what caused it.
 *
 * CANCEL_REJECTED is separate from REJECTED on purpose. Rejecting an *order*
 * means it never entered the book; declining a *cancel request* leaves an order
 * that is alive and well, or that filled and left. Emporia's
 * `OrderLifecycle.tla` models the second as `DeclineCancelAfterFill` — a step
 * that resolves the pending request while leaving `status` and `filled`
 * untouched.
 */
enum class OutcomeType : std::uint8_t {
	EXCHANGE_ENUM_VALUES(OUTCOME_TYPE_LIST)
};

EXCHANGE_ENUM_NAME(OutcomeType, to_string, OUTCOME_TYPE_LIST)

/**
 * @brief One observable step in an order's life, produced by @c order_book.
 *
 * Fills the gap Emporia's model exposes: before this, @c Trade was the engine's
 * only output, so a fill-or-kill that could not fill, an IOC remainder and a
 * cancel for an unknown id were all silent. Every one of those now produces a
 * record, and every record names the order it concerns.
 *
 * Sits beside @c Trade rather than in @c event/ for the same reason @c Trade
 * does: this is matching output, a value the book appends to a caller's buffer.
 * The event-sourcing record that a persistence layer would durably log is a
 * separate type in @c event/, above this one in the layer graph.
 *
 * @par Reading traded / remaining
 * They describe the order *after* this outcome, so a client can reconstruct the
 * whole lifecycle from the stream alone. The exception is CANCEL_REJECTED,
 * where the book has no record of the order to report — it filled and left, or
 * never existed — so @c status is NEW and the quantities are zero. Only @c id
 * and @c reason carry information there, which is the honest answer: the engine
 * genuinely cannot distinguish "filled a microsecond ago" from "never placed",
 * because both leave the same empty index.
 *
 * @note Trivially copyable and 32 bytes, so a batch of these moves through the
 *       same memcpy paths as @c Trade and @c event::Command.
 */
struct OrderOutcome {
	order_id_t id;        ///< the order this concerns
	OutcomeType type;     ///< what happened
	reject_reason reason;  ///< NONE unless type is REJECTED or CANCEL_REJECTED
	OrderStatus status;   ///< the order's status after this outcome
	quantity_t traded;    ///< cumulative executed quantity, after this outcome
	quantity_t remaining; ///< unexecuted quantity, after this outcome

	/// @brief The book accepted @p id; nothing executed yet.
	[[nodiscard]] TRADING_ENGINE_EXPORT static OrderOutcome
	accepted(order_id_t id, quantity_t quantity) noexcept;

	/// @brief @p id never entered the book.
	[[nodiscard]] TRADING_ENGINE_EXPORT static OrderOutcome
	rejected(order_id_t id, reject_reason reason, quantity_t quantity) noexcept;

	/// @brief Quantity executed against @p id, leaving it in @p state.
	[[nodiscard]] TRADING_ENGINE_EXPORT static OrderOutcome
	fill(order_id_t id, const order_state &state) noexcept;

	/// @brief @p id's remainder was withdrawn, leaving it in @p state.
	/// @param reason NONE for a client cancel, TIME_IN_FORCE for an IOC drop.
	[[nodiscard]] TRADING_ENGINE_EXPORT static OrderOutcome
	cancelled(order_id_t id, const order_state &state,
			  reject_reason reason = reject_reason::NONE) noexcept;

	/// @brief A cancel request for @p id could not be applied.
	[[nodiscard]] TRADING_ENGINE_EXPORT static OrderOutcome
	cancel_rejected(order_id_t id, reject_reason reason) noexcept;

	bool operator==(const OrderOutcome &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<OrderOutcome>,
			  "OrderOutcome must stay trivially copyable so batches of it move "
			  "through the same memcpy paths as Trade and Command");

} // namespace exchange::engine
