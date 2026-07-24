#pragma once
#include "order_book/order.hpp"
#include "order_book/side.hpp"
#include "order_book/types.hpp"

#include <cstdint>
#include <type_traits>

namespace event {

// Command/LevelChange are execution input; they name order_book domain types
// (a downward dependency — Event sits above OrderBook in the layer graph).
using order_book::Order;
using order_book::OrderId;
using order_book::Price;
using order_book::Side;
using order_book::Volume;

/// @brief Side/price/volume payload shared by ADD, REDUCE and SET_LEVEL.
struct LevelChange {
	Side side;
	Price price;
	Volume volume;
};

/**
 * @brief One unit of work handed to the matching engine over the SPSC queue: a
 *        tag plus the payload for exactly one book mutation.
 *
 * Deliberately a trivially copyable tagged union so @c spsc_queue<Command, N>
 * takes its @c memcpy batch path. There is no default constructor — build one
 * with a named factory (@c Command::place, @c Command::cancel, …) so the
 * union's active member always matches @c type.
 */
struct [[nodiscard]] Command {
	/// @brief Which book mutation a Command carries.
	enum class Type : std::uint8_t {
		PLACE,     ///< place_order: cross, then rest the remainder
		CANCEL,    ///< cancel_order: remove a resting order by id
		ADD,       ///< add_order: rest anonymous liquidity, no matching
		REDUCE,    ///< delete_order: drain volume at a price, FIFO-first
		SET_LEVEL, ///< set_level: overwrite the absolute L2 size at a price
	};

	Type type;

	union {
		Order order;       ///< PLACE
		OrderId cancel_id; ///< CANCEL
		LevelChange level; ///< ADD / REDUCE / SET_LEVEL
	};

	TRADING_ENGINE_EXPORT static Command place(const Order &o) noexcept;
	TRADING_ENGINE_EXPORT static Command cancel(OrderId id) noexcept;
	TRADING_ENGINE_EXPORT static Command add(Side side, Price price,
	                                         Volume volume) noexcept;
	TRADING_ENGINE_EXPORT static Command reduce(Side side, Price price,
	                                            Volume volume) noexcept;
	TRADING_ENGINE_EXPORT static Command set_level(Side side, Price price,
	                                               Volume volume) noexcept;

private:
	// Each ctor initialises exactly the union member that matches the tag, so
	// reading it back through the same tag is always the active member.
	explicit Command(const Order &o) noexcept;
	Command(Type t, OrderId id) noexcept;
	Command(Type t, LevelChange lc) noexcept;
};

static_assert(
	std::is_trivially_copyable_v<Command>,
	"Command must stay trivially copyable for the lockfree's memcpy path");

} // namespace event
