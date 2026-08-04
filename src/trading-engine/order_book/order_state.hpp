#pragma once
#include "core/types.hpp"
#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine {

#define ORDER_STATUS_LIST(X)                                                   \
	X(NEW, "validated but not yet accepted by the book")                       \
	X(LIVE, "accepted, resting, nothing executed yet")                         \
	X(PARTIALLY_FILLED, "some quantity executed, some still resting")          \
	X(FILLED, "fully executed; terminal")                                      \
	X(CANCELLED, "withdrawn with quantity still unexecuted; terminal")         \
	X(REJECTED, "never entered the book; terminal")

/// @brief Where an order sits in its lifecycle.
///
/// NEW and REJECTED belong to the validation boundary rather than to the book:
/// an order is NEW only until @c place_order decides, and a REJECTED one never
/// gets an @c order_state at all. Both are still reportable on the outcome
/// stream, which is why they are enumerated here and not only in @c order_state.
enum class OrderStatus : std::uint8_t {
	EXCHANGE_ENUM_VALUES(ORDER_STATUS_LIST)
};

/// @brief The enumerator name of an @c OrderStatus, e.g. @c "PARTIALLY_FILLED".
EXCHANGE_ENUM_NAME(OrderStatus, to_string, ORDER_STATUS_LIST)

/// @brief LIVE or PARTIALLY_FILLED — the order can still fill or be cancelled.
[[nodiscard]] constexpr bool is_active(OrderStatus s) noexcept {
	return s == OrderStatus::LIVE || s == OrderStatus::PARTIALLY_FILLED;
}

/// @brief FILLED, CANCELLED or REJECTED — no transition may leave this state.
[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
	return s == OrderStatus::FILLED || s == OrderStatus::CANCELLED ||
		   s == OrderStatus::REJECTED;
}

/**
 * @brief The quantity/status state machine of one order.
 *
 * @par The invariants
 * ```
 * quantity > 0
 * 0 <= traded && traded <= quantity
 * remaining == quantity - traded
 * status == LIVE             ==> traded == 0 && remaining == quantity
 * status == PARTIALLY_FILLED ==> traded >  0 && remaining >  0
 * status == FILLED           ==> traded == quantity && remaining == 0
 * status == CANCELLED        ==> traded <  quantity && remaining >  0
 * ```
 *
 * @par Why status is derived rather than stored
 * A stored status has to be kept in step with the quantities on every
 * operation, and every such pairing is a bug waiting to happen. Here the class
 * stores @c quantity_, @c remaining_ and a single @c cancelled_ bit, and
 * @c status() computes the rest — the implications above are then unfalsifiable
 * rather than merely maintained, because no representable state breaks them.
 *
 * Cancellation is the one bit the quantities genuinely cannot express, since
 * `CANCELLED ==> traded < quantity && remaining > 0` overlaps exactly with LIVE
 * and PARTIALLY_FILLED. That is what @c cancelled_ is for.
 *
 * @note Trivially copyable and 24 bytes: one of these is embedded in every
 *       @c detail::resting_order, and therefore in every pool node.
 */
class order_state {
public:
	/**
	 * @brief An order of @p initial_quantity with nothing executed — LIVE.
	 * @param initial_quantity Order quantity in lots. Must be positive; the
	 *        caller validates and rejects first (@c order_book emits REJECTED /
	 *        NON_POSITIVE_QUANTITY), because there is no representable
	 *        @c order_state for a non-positive order.
	 */
	TRADING_ENGINE_EXPORT explicit order_state(
		quantity_t initial_quantity) noexcept;

	/// @brief Execute @p lots against this order.
	/// @pre The order is active and @c 0 < lots <= remaining() — an overfill is
	///      a caller bug, not a case to clamp.
	TRADING_ENGINE_EXPORT void apply_fill(quantity_t lots) noexcept;

	/**
	 * @brief Resize the order to @p new_quantity, keeping executed quantity.
	 *
	 * @warning Quantity only. Time priority is a book concern this type cannot
	 *          see: an increase must lose priority and a decrease must keep it,
	 *          which is a decision about where the node sits in its level's
	 *          FIFO. A downsize to at or below @c traded() is a cancel, and
	 *          violates the precondition rather than silently clamping.
	 */
	TRADING_ENGINE_EXPORT void modify(quantity_t new_quantity) noexcept;

	/**
	 * @brief Withdraw the unexecuted remainder — terminal.
	 *
	 * Freezes the executed quantity rather than discarding it: a cancellation
	 * withdraws what is left, it does not undo the fills.
	 * @pre The order is active. An order that filled first is already terminal,
	 *      and its cancel request must be declined by the caller.
	 */
	TRADING_ENGINE_EXPORT void cancel() noexcept;

	/// @brief The initial quantity, or the latest @c modify.
	[[nodiscard]] quantity_t quantity() const noexcept { return quantity_; }

	/// @brief Cumulative executed quantity. Never decreases.
	[[nodiscard]] quantity_t traded() const noexcept {
		return quantity_ - remaining_;
	}

	/// @brief Unexecuted quantity still resting.
	[[nodiscard]] quantity_t remaining() const noexcept { return remaining_; }

	/// @brief The derived status. @see the class note on why it is not stored.
	[[nodiscard]] TRADING_ENGINE_EXPORT OrderStatus status() const noexcept;

	/// @brief Can still fill or be cancelled (LIVE or PARTIALLY_FILLED).
	[[nodiscard]] bool is_active() const noexcept {
		return engine::is_active(status());
	}

	bool operator==(const order_state &) const noexcept = default;

private:
	quantity_t quantity_;
	quantity_t remaining_;
	bool cancelled_ = false;
};

} // namespace exchange::engine
