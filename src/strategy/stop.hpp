#pragma once
// Stop: hold an order back until the tape trades through its trigger.

#include "command_writer.hpp"
#include "detail/armed_stop.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/order_type.hpp"
#include "trading-engine/orders/types.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace exchange::strategy {

/**
 * @brief Holds armed stop orders and releases them when the market prints
 *        through their trigger.
 *
 * @tparam MaxArmed Stops this instance can hold at once.
 *
 * @par Why the book cannot do this
 * @c order_book refuses @c order_type::STOP outright with
 * @c UNSUPPORTED_ORDER_TYPE, and that refusal is deliberate rather than a gap:
 * a stop resting in a book is live, and an order that is live the instant it
 * arrives is not a stop. Triggering needs something watching the tape, and the
 * tape is an output of matching - so a book that watched it would be reading its
 * own output, which is the cycle the layering exists to prevent. Out here it is
 * a plain feedback loop: trades in, commands out.
 *
 * @par Trigger rule
 * A buy stop fires when the market trades at or above its trigger, a sell stop
 * at or below. Both use the print price rather than a quote, because a trade is
 * the only thing this strategy is given and the only thing that is unambiguous:
 * a quote can flicker, a print happened.
 *
 * @par What is released
 * A LIMIT at the armed order's price - a stop-limit. The trigger price is
 * cleared on the way out, since the released order is not a stop any more and
 * carrying a trigger past it would be a contradiction the validation boundary
 * catches (@c UNEXPECTED_STOP_PRICE). A stop-*market* would need the released
 * order to sweep, which is a decision about the order and not about the trigger;
 * arm a marketable limit if that is what is wanted.
 *
 * @par Slippage is real and is not hidden
 * The release is a command, so it queues behind whatever is already in flight
 * and reaches the book after the print that triggered it. It can therefore fill
 * worse than the trigger, or not at all. That is what a stop is on a real venue;
 * the alternative - filling at the trigger price - would be inventing liquidity.
 *
 * @note One print can trigger every armed stop at once, so the bound is
 *       @p MaxArmed rather than 1. That is the honest number, and it is not
 *       free: the host sizes its buffer from it, so a generous @p MaxArmed buys
 *       capacity with producer-side bytes. Arm what you need.
 */
template <std::size_t MaxArmed = 8>
class stop {
public:
	static constexpr std::size_t MAX_COMMANDS_PER_EVENT = MaxArmed;
	static constexpr std::size_t MAX_ARMED              = MaxArmed;

	/**
	 * @brief Hold @p o back until the market trades through its trigger.
	 *
	 * Emits nothing - that is the point of a stop.
	 *
	 * @param o A @c STOP order: @c type must be @c order_type::STOP, @c id and
	 *        @c qty positive, and @c stop_price non-zero. Its @c symbol_id is
	 *        ignored; the host stamps the listing on release.
	 * @return @c false if @p o is not a well-formed stop, an order with that id
	 *         is already armed, or every slot is taken.
	 */
	bool arm(const engine::orders::order &o) noexcept {
		if (o.type != engine::orders::order_type::STOP) return false;
		if (o.id == 0 || o.qty <= 0 || o.stop_price == 0) return false;
		if (find(o.id) != nullptr) return false;

		detail::armed_stop *slot = free_slot();
		if (slot == nullptr) return false;

		slot->resting = o;
		slot->active  = true;
		++armed_;
		return true;
	}

	/**
	 * @brief Withdraw an armed stop before it triggers.
	 * @return @c false if no stop with that id is armed.
	 * @note No CANCEL is sent, because nothing was ever placed. A stop that has
	 *       already triggered is gone from here and must be cancelled through
	 *       the book by its id, like any other resting order.
	 */
	bool disarm(order_id_t id) noexcept {
		detail::armed_stop *slot = find(id);
		if (slot == nullptr) return false;
		slot->active = false;
		--armed_;
		return true;
	}

	/// @brief Release every stop @p t triggers.
	void on_trade(const engine::trade &t, command_writer &out) noexcept {
		for (detail::armed_stop &slot : slots_) {
			if (!slot.active) continue;
			if (!triggers(slot.resting, t.price)) continue;

			engine::orders::order released = slot.resting;
			released.type          = engine::orders::order_type::LIMIT;
			released.stop_price    = 0;
			slot.active            = false;
			--armed_;
			out.place(released);
		}
	}

	/// @brief Stops currently held.
	[[nodiscard]] constexpr std::size_t armed() const noexcept {
		return armed_;
	}

	/// @brief The order @p id would release, or nothing if it is not armed.
	[[nodiscard]] std::optional<engine::orders::order>
	pending(order_id_t id) const noexcept {
		const detail::armed_stop *slot = find(id);
		if (slot == nullptr) return std::nullopt;
		return slot->resting;
	}

	/// @brief Whether a print at @p price would fire @p o.
	/// @note Exposed because it is the whole rule, and a rule worth testing on
	///       its own rather than only through the fan-out.
	[[nodiscard]] static constexpr bool triggers(const engine::orders::order &o,
												price_t price) noexcept {
		return o.side == side_t::bid ? price >= o.stop_price
									 : price <= o.stop_price;
	}

private:
	[[nodiscard]] detail::armed_stop *free_slot() noexcept {
		for (detail::armed_stop &slot : slots_)
			if (!slot.active) return &slot;
		return nullptr;
	}

	[[nodiscard]] detail::armed_stop *find(order_id_t id) noexcept {
		for (detail::armed_stop &slot : slots_)
			if (slot.active && slot.resting.id == id) return &slot;
		return nullptr;
	}

	[[nodiscard]] const detail::armed_stop *find(order_id_t id) const noexcept {
		for (const detail::armed_stop &slot : slots_)
			if (slot.active && slot.resting.id == id) return &slot;
		return nullptr;
	}

	std::array<detail::armed_stop, MaxArmed> slots_{};
	std::size_t armed_ = 0;
};

} // namespace exchange::strategy
