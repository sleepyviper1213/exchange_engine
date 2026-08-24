#pragma once
#include "fwd.hpp"
#include "order_state.hpp"
#include "outcome_type.hpp"          // IWYU pragma: export
#include "reject_reason.hpp"
#include "orders/types.hpp"
#include "order_book_export.hpp" // ORDER_BOOK_EXPORT (generated)

#include <type_traits>

namespace exchange::engine {

/**
 * @brief One observable step in an order's life, produced by @c order_book.
 *
 * @c trade alone cannot report an order's fate: a fill-or-kill that could not
 * fill, an IOC remainder and a cancel for an unknown id all execute nothing and
 * would otherwise be silent. Every one of those produces a record here, and
 * every record names the order it concerns.
 *
 * @par Reading traded / remaining
 * They describe the order *after* this outcome, so a client can reconstruct a
 * whole lifecycle from the stream alone. The exception is CANCEL_REJECTED,
 * where the book has no record of the order to report - it filled and left, or
 * never existed - so @c status is NEW and the quantities are zero. Only @c id
 * and @c reason carry information there, which is the honest answer: the engine
 * genuinely cannot tell "filled a microsecond ago" from "never placed", because
 * both leave the same empty index.
 *
 * @note Trivially copyable and 32 bytes, so a batch of these moves through the
 *       same memcpy paths as @c trade and @c event::command.
 */
struct order_outcome {
	order_id_t id;        ///< the order this concerns
	OutcomeType type;     ///< what happened
	reject_reason reason; ///< NONE unless type is REJECTED or CANCEL_REJECTED
	OrderStatus status;   ///< the order's status after this outcome
	quantity_t traded;    ///< cumulative executed quantity, after this outcome
	quantity_t remaining; ///< unexecuted quantity, after this outcome

	/// @brief The book accepted @p id; nothing executed yet.
	[[nodiscard]] ORDER_BOOK_EXPORT static order_outcome
	accepted(order_id_t id, quantity_t quantity) noexcept;

	/// @brief @p id never entered the book.
	[[nodiscard]] ORDER_BOOK_EXPORT static order_outcome
	rejected(order_id_t id, reject_reason reason, quantity_t quantity) noexcept;

	/// @brief Quantity executed against @p id, leaving it in @p state.
	[[nodiscard]] ORDER_BOOK_EXPORT static order_outcome
	fill(order_id_t id, const order_state &state) noexcept;

	/// @brief @p id's remainder was withdrawn, leaving it in @p state.
	/// @param reason NONE for a client cancel, TIME_IN_FORCE for an IOC drop.
	[[nodiscard]] ORDER_BOOK_EXPORT static order_outcome
	cancelled(order_id_t id, const order_state &state,
			  reject_reason reason = reject_reason::NONE) noexcept;

	/// @brief A cancel request for @p id could not be applied.
	[[nodiscard]] ORDER_BOOK_EXPORT static order_outcome
	cancel_rejected(order_id_t id, reject_reason reason) noexcept;

	bool operator==(const order_outcome &) const noexcept = default;
};

static_assert(
	std::is_trivially_copyable_v<order_outcome>,
	"order_outcome must stay trivially copyable so batches of it move "
	"through the same memcpy paths as trade and command");

} // namespace exchange::engine
