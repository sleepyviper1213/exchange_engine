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

/**
 * @brief Where an order sits in its lifecycle.
 *
 * Ported from Emporia's `OrderLifecycle.tla` (`Statuses`), which is the wider
 * of its two models: the KeY/JML `OrderStateModel` covers only LIVE,
 * PARTIALLY_FILLED, FILLED and CANCELLED, because a rejected order never
 * reaches the state machine and NEW is not observable once construction
 * finishes. Both are kept here — NEW and REJECTED are reachable on the
 * validation path in @c order_book::place_order, which the Java model has no
 * equivalent of.
 */
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
 * @brief The quantity/status state machine of one order, ported from Emporia's
 *        machine-checked `OrderStateModel`.
 *
 * @par Provenance
 * Emporia carries two verification artifacts for this, and this type is the
 * union of what they say:
 *
 * - `verification/key-order-state/OrderStateModel.java` — a dependency-free
 *   Java model whose JML contracts (class invariants plus a pre/postcondition
 *   pair per operation) are discharged by KeY. That fixes the *arithmetic*:
 *   what `traded`, `remaining` and `status` must be after a fill, a modify and
 *   a cancel.
 * - `verification/order-lifecycle/OrderLifecycle.tla` — a TLA+ model checked by
 *   TLC. That fixes the *temporal* part: terminal states never change, no
 *   execution follows a confirmed cancel, and a pending cancel always resolves
 *   (the fill/cancel race). See this repo's `verification/order-lifecycle/`.
 *
 * @par The invariants, verbatim from the JML
 * ```
 * quantity > 0
 * 0 <= traded && traded <= quantity
 * remaining == quantity - traded
 * remaining >= 0
 * status == LIVE             ==> traded == 0 && remaining == quantity
 * status == PARTIALLY_FILLED ==> traded >  0 && remaining >  0
 * status == FILLED           ==> traded == quantity && remaining == 0
 * status == CANCELLED        ==> traded <  quantity && remaining >  0
 * ```
 *
 * @par Why status is derived rather than stored
 * Emporia stores `status` in a field and needs four JML clauses to keep it
 * agreeing with the quantities; KeY then proves those clauses hold after every
 * operation. Here the same agreement is structural: the class stores
 * @c quantity_, @c remaining_ and a single @c cancelled_ bit, and @c status()
 * computes the rest. The four implications above are then unfalsifiable rather
 * than merely proved — there is no representable state that breaks them, so
 * there is nothing for a proof or an assertion to discharge.
 *
 * The one bit the quantities genuinely cannot express is cancellation, since
 * `CANCELLED ==> traded < quantity && remaining > 0` overlaps exactly with LIVE
 * and PARTIALLY_FILLED. That is what @c cancelled_ is, and it is why @c cancel
 * carries the only precondition the compiler cannot enforce.
 *
 * @par What is deliberately not modelled
 * NEW and REJECTED are absent from this type. A rejected order never gets an
 * @c order_state (rejection happens during validation, before construction) and
 * NEW lasts only until the constructor returns. Both are reachable as an
 * @c OrderStatus on the outcome stream; neither is a state this object can hold
 * — which is the same narrowing Emporia's JML model makes.
 *
 * @note Trivially copyable and 24 bytes, because one of these is embedded in
 *       every @c detail::resting_order and therefore in every pool node.
 */
class order_state {
public:
	/**
	 * @brief An order of @p initial_quantity with nothing executed — LIVE.
	 *
	 * JML: `requires initial_quantity > 0; ensures traded == 0; ensures
	 * remaining == initial_quantity; ensures status == LIVE`.
	 *
	 * @param initial_quantity Order quantity in lots. Must be positive; the
	 *        caller validates and rejects before constructing (@c order_book
	 *        emits REJECTED / NON_POSITIVE_QUANTITY), because there is no
	 *        representable @c order_state for a non-positive order.
	 */
	TRADING_ENGINE_EXPORT explicit order_state(
		quantity_t initial_quantity) noexcept;

	/**
	 * @brief Execute @p lots against this order.
	 *
	 * JML: `requires status == LIVE || status == PARTIALLY_FILLED; requires
	 * lots > 0; requires lots <= remaining; ensures traded == \old(traded) +
	 * lots; ensures remaining == quantity - traded; ensures remaining == 0 ==>
	 * status == FILLED; ensures remaining > 0 ==> status == PARTIALLY_FILLED`.
	 *
	 * The two postconditions on status are the derivation in @c status(), so
	 * only the preconditions are checked here.
	 */
	TRADING_ENGINE_EXPORT void apply_fill(quantity_t lots) noexcept;

	/**
	 * @brief Resize the order to @p new_quantity, keeping executed quantity.
	 *
	 * JML: `requires status == LIVE || status == PARTIALLY_FILLED; requires
	 * new_quantity > traded; ensures quantity == new_quantity; ensures traded
	 * == \old(traded); ensures remaining == new_quantity - traded; ensures
	 * status == \old(status)`.
	 *
	 * @warning Quantity only. Time priority is a book concern and this type
	 *          cannot see it: a size increase must lose priority and a decrease
	 *          must keep it, which is @c order_book's decision about where the
	 *          node goes in its level's FIFO, not this object's. Emporia's
	 *          simulator gets that wrong (`OrderBookImpl.modifyOrder` adjusts
	 * in place either way); do not copy it. A downsize to at or below
	 *          @c traded is a cancel, so it violates the precondition here
	 *          rather than silently clamping.
	 */
	TRADING_ENGINE_EXPORT void modify(quantity_t new_quantity) noexcept;

	/**
	 * @brief Withdraw the unexecuted remainder — terminal.
	 *
	 * JML: `requires status == LIVE || status == PARTIALLY_FILLED; ensures
	 * quantity, traded and remaining unchanged; ensures status == CANCELLED`.
	 *
	 * Cancelling freezes the executed quantity rather than discarding it, which
	 * is the TLA+ property `NoExecutionAfterCancellation`. The precondition is
	 * the other half of the fill/cancel race: an order that filled first is
	 * FILLED and terminal, and its cancel request must be declined by the
	 * caller (`DeclineCancelAfterFill`) rather than applied here.
	 */
	TRADING_ENGINE_EXPORT void cancel() noexcept;

	/// @brief Current order quantity — the initial quantity, or the latest
	///        @c modify.
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
