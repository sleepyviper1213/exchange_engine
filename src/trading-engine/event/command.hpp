#pragma once
#include "../order_book/order.hpp"
#include "fwd.hpp"

#include <cstdint>
#include <type_traits>

namespace exchange::engine::event {

// Command/LevelChange are execution input; they name order_book domain types
// (a downward dependency — Event sits above OrderBook in the layer graph).
using exchange::engine::Order;

/// @brief Side/price/qty payload shared by ADD, REDUCE and SET_LEVEL.
struct LevelChange {
	side_t side;
	price_t price;
	quantity_t volume;
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
struct Command {
	/// @brief Which book mutation a Command carries.
	enum class Type : std::uint8_t {
		PLACE,     ///< place_order: cross, then rest the remainder
		CANCEL,    ///< cancel_order: remove a resting order by id
		ADD,       ///< add_order: rest anonymous liquidity, no matching
		REDUCE,    ///< delete_order: drain qty at a price, FIFO-first
		SET_LEVEL, ///< set_level: overwrite the absolute L2 size at a price
	};

	Type type;

	union {
		Order order;       ///< PLACE
		order_id_t cancel_id; ///< CANCEL
		LevelChange level; ///< ADD / REDUCE / SET_LEVEL
	};

	TRADING_ENGINE_EXPORT static Command place(const Order &o) noexcept;
	TRADING_ENGINE_EXPORT static Command cancel(order_id_t id) noexcept;
	TRADING_ENGINE_EXPORT static Command add(side_t side, price_t price,
	                                         quantity_t volume) noexcept;
	TRADING_ENGINE_EXPORT static Command reduce(side_t side, price_t price,
	                                            quantity_t volume) noexcept;
	TRADING_ENGINE_EXPORT static Command set_level(side_t side, price_t price,
	                                               quantity_t volume) noexcept;

private:
	// Each ctor initialises exactly the union member that matches the tag, so
	// reading it back through the same tag is always the active member.
	explicit Command(const Order &o) noexcept;
	Command(Type t, order_id_t id) noexcept;
	Command(Type t, LevelChange lc) noexcept;
};

static_assert(
	std::is_trivially_copyable_v<Command>,
	"Command must stay trivially copyable for the lockfree's memcpy path");

} // namespace event
