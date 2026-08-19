#pragma once
#include "../orders/order.hpp"
#include "../orders/types.hpp"
#include "fwd.hpp"
#include "trading_engine_export.hpp" // TRADING_ENGINE_EXPORT (generated)

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace exchange::engine::event {

// Command/level_change are execution input; they name order domain types
// (a downward dependency - Event sits above Orders in the layer graph).
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
 * takes its @c memcpy batch path. There is no default constructor - build one
 * with a named factory (@c Command::place, @c Command::cancel, …) so the
 * union's active member always matches @c type.
 */
struct command {
	/// @brief Which book mutation a Command carries.
	enum class Type : std::uint8_t {
		PLACE,  ///< place_order: cross, then rest the remainder
		CANCEL, ///< cancel_order: remove a resting order by id
		ADD,    ///< add_order: rest anonymous liquidity, no matching
		REDUCE, ///< delete_order: drain qty at a price, FIFO-first
	};

	Type type;

	/**
	 * @brief Which listing this command is for - the routing key.
	 *
	 * On the command rather than in the union because @c dispatcher has to read
	 * it for every command without first switching on the tag, and three of the
	 * four payloads have nowhere to put it: a @c level_change is a side, a
	 * price and a size, and a CANCEL is an id. Only PLACE carried a symbol,
	 * inside its
	 * @c order, which made exactly one of four command types routable.
	 *
	 * @note Free, as it happens. The tag is one byte followed by seven of
	 *       padding, because the union aligns to eight; the symbol lands in
	 * that padding and @c sizeof(command) does not move.
	 * @note Zero is "unspecified", matching @c order::symbol_id - the honest
	 *       value for a single-book deployment that never routes.
	 */
	symbol_id_t symbol;

	/**
	 * @brief The order a PLACE carries.
	 * @pre @c type is @c Type::PLACE. Reading the wrong arm of the union is
	 *      undefined behaviour, not a misread value, so this is checked rather
	 *      than trusted - the assert survives an optimised build under
	 *      @c enable_hardening.
	 */
	[[nodiscard]] const order &as_place() const noexcept {
		assert(type == Type::PLACE);
		return order_; // NOLINT(cppcoreguidelines-pro-type-union-access)
	}

	/// @brief The order id a CANCEL names. @pre @c type is @c Type::CANCEL.
	[[nodiscard]] order_id_t as_cancel() const noexcept {
		assert(type == Type::CANCEL);
		return cancel_id; // NOLINT(cppcoreguidelines-pro-type-union-access)
	}

	/// @brief The side/price/size an ADD or REDUCE carries.
	/// @pre @c type is @c Type::ADD or @c Type::REDUCE.
	[[nodiscard]] const level_change &as_level() const noexcept {
		assert(type == Type::ADD || type == Type::REDUCE);
		return level; // NOLINT(cppcoreguidelines-pro-type-union-access)
	}

	/// @brief Place @p o. The symbol is taken from @c order::symbol_id, which
	/// is
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
	/**
	 * @brief Exactly one book mutation's payload, picked by @c type.
	 *
	 * Private, and reached only through @c as_place / @c as_cancel /
	 * @c as_level. A union whose arms could be read from anywhere puts the
	 * tag-matches-payload obligation on every call site and gives undefined
	 * behaviour to whichever one forgets; confining it here leaves three
	 * checked readers and three writers, all in this file, and no way to ask
	 * the question wrongly.
	 *
	 * @note Still a union rather than a @c std::variant, which is what the
	 *       Core Guidelines would otherwise ask for. A variant carries its own
	 *       discriminant beside the @c type this already has, and - decisively
	 *       - @c spsc_queue's batch @c memcpy path needs the whole command
	 *       trivially copyable, which the @c static_assert below enforces.
	 */
	union {
		order order_;         ///< PLACE
		order_id_t cancel_id; ///< CANCEL
		level_change level;   ///< ADD / REDUCE
	};

	// Each ctor initialises exactly the union member that matches the tag, so
	// reading it back through the same tag is always the active member.
	explicit command(const order &o) noexcept;
	command(Type t, symbol_id_t listing, order_id_t id) noexcept;
	command(Type t, symbol_id_t listing, level_change lc) noexcept;
};

static_assert(
	std::is_trivially_copyable_v<command>,
	"Command must stay trivially copyable for the lockfree's memcpy path");

} // namespace exchange::engine::event
