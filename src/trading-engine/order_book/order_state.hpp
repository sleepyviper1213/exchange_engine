#pragma once
#include "fwd.hpp"
#include "order_status.hpp"          // IWYU pragma: export
#include "trading-engine/orders/types.hpp"
#include "trading_engine_export.hpp" // TRADING_ENGINE_EXPORT (generated)

#include <cstdint>
#include <type_traits>

namespace exchange::engine {


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
 * and PARTIALLY_FILLED. That is what @c cancelled_ is for — and it is stored as
 * an actual bit, not a @c bool, because a @c bool next to two quantities costs
 * eight bytes of padding and this type is embedded in every pool node.
 *
 * @note Trivially copyable and 8 bytes: one of these sits in every
 *       @c detail::resting_order, and it is what takes a node to 32 bytes and
 *       therefore two to a cache line. The quantity gives up its sign bit to
 *       carry the cancellation flag, which costs nothing — an order's quantity
 *       is positive by invariant, so 31 bits is the same usable range as the
 *       signed 32 that @c quantity_t offers.
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
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity_t quantity() const noexcept;

	/// @brief Cumulative executed quantity. Never decreases.
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity_t traded() const noexcept;

	/// @brief Unexecuted quantity still resting.
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity_t remaining() const noexcept;

	/// @brief The derived status. @see the class note on why it is not stored.
	[[nodiscard]] TRADING_ENGINE_EXPORT TRADING_ENGINE_EXPORT OrderStatus status() const noexcept;

	/// @brief Can still fill or be cancelled (LIVE or PARTIALLY_FILLED).
	[[nodiscard]] TRADING_ENGINE_EXPORT bool is_active() const noexcept;

	bool operator==(const order_state &) const noexcept = default;

private:
	/// @brief Bit 31 of @c quantity_and_flag_: set once the order is cancelled.
	static constexpr std::uint32_t CANCELLED_BIT = 1U << 31U;
	/// @brief The low 31 bits, which hold the quantity. Lossless because an
	///        order's quantity is positive by invariant, so the sign bit
	///        @c quantity_t would have spent was never carrying anything.
	static constexpr std::uint32_t QUANTITY_MASK = CANCELLED_BIT - 1U;

	/// @brief Quantity in the low 31 bits, cancellation in the top one.
	///
	/// Packed by hand rather than declared as two bitfields: bitfield layout is
	/// implementation-defined, and this type carries a @c static_assert on its
	/// own size that the matching loop's cache behaviour depends on. An
	/// explicit mask and bit say the same thing in a way the ABI cannot
	/// reinterpret.
	std::uint32_t quantity_and_flag_;

	/// @brief Unexecuted quantity, kept as a plain signed word rather than
	///        joined to the pack: this is the field the matching loop reads on
	///        every fill, and masking it would put an AND on that path.
	quantity_t remaining_;
};

// This is the size that makes detail::resting_order 32 bytes, which is what
// puts two of them on a cache line. Anything added here comes out of that.
static_assert(sizeof(order_state) == 8,
			  "order_state must stay one 64-bit word — see resting_order");
static_assert(std::is_trivially_copyable_v<order_state>,
			  "order_state is copied by value onto pool nodes");

} // namespace exchange::engine
