#pragma once
#include "../orders/order.hpp"
#include "../orders/types.hpp"
#include "fwd.hpp"

#include <cstdint>
#include <type_traits>

namespace exchange::engine::event {

// Command/level_change are execution input; they name order domain types
// (a downward dependency — Event sits above Orders in the layer graph).
using exchange::engine::orders::order;

/// @brief Side/price/qty payload shared by ADD and REDUCE.
struct level_change {
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
struct command {
	/// @brief Which book mutation a Command carries.
	enum class Type : std::uint8_t {
		PLACE,     ///< place_order: cross, then rest the remainder
		CANCEL,    ///< cancel_order: remove a resting order by id
		ADD,       ///< add_order: rest anonymous liquidity, no matching
 		REDUCE,    ///< delete_order: drain qty at a price, FIFO-first
	};

	Type type;

	/**
	 * @brief Which listing this command is for — the routing key.
	 *
	 * On the command rather than in the union because @c dispatcher has to read
	 * it for every command without first switching on the tag, and three of the
	 * four payloads have nowhere to put it: a @c level_change is a side, a price
	 * and a size, and a CANCEL is an id. Only PLACE carried a symbol, inside its
	 * @c order, which made exactly one of four command types routable.
	 *
	 * @note Free, as it happens. The tag is one byte followed by seven of
	 *       padding, because the union aligns to eight; the symbol lands in that
	 *       padding and @c sizeof(command) does not move.
	 * @note Zero is "unspecified", matching @c order::symbol_id — the honest
	 *       value for a single-book deployment that never routes.
	 */
	symbol_id_t symbol;

	union {
		order order_;       ///< PLACE
		order_id_t cancel_id; ///< CANCEL
		level_change level; ///< ADD / REDUCE
	};

	/// @brief Place @p o. The symbol is taken from @c order::symbol_id, which is
	///        where a validated order already records it.
	TRADING_ENGINE_EXPORT static command place(const order &o) noexcept;
	TRADING_ENGINE_EXPORT static command cancel(symbol_id_t symbol,
												order_id_t id) noexcept;
	TRADING_ENGINE_EXPORT static command add(symbol_id_t symbol, side_t side,
											 price_t price,
	                                         quantity_t volume) noexcept;
	TRADING_ENGINE_EXPORT static command reduce(symbol_id_t symbol, side_t side,
												price_t price,
	                                            quantity_t volume) noexcept;

private:
	// Each ctor initialises exactly the union member that matches the tag, so
	// reading it back through the same tag is always the active member.
	explicit command(const order &o) noexcept;
	command(Type t, symbol_id_t symbol, order_id_t id) noexcept;
	command(Type t, symbol_id_t symbol, level_change lc) noexcept;
};

static_assert(
	std::is_trivially_copyable_v<command>,
	"Command must stay trivially copyable for the lockfree's memcpy path");

} // namespace event
