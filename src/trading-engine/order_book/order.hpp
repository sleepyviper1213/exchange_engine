#pragma once
#include "core/types.hpp"
#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine {

// TODO: Future MARKET / LIMIT / STOP types are added here.

#define ORDER_TYPE_LIST(X)                                                     \
	X(GOOD_TILL_CANCELLED, "rest the unfilled remainder indefinitely")         \
	X(FILL_OR_KILL, "execute fully and immediately, or not at all")            \
	X(IMMEDIATE_OR_CANCEL, "execute what crosses now, drop the remainder")

/// @brief Time-in-force / execution policy for an incoming order.
enum class OrderType : std::uint8_t {
	EXCHANGE_ENUM_VALUES(ORDER_TYPE_LIST)
};

/// @brief The enumerator name of an @c OrderType, e.g. @c "FILL_OR_KILL"
///        (empty view if out of range).
EXCHANGE_ENUM_NAME(OrderType, to_string, ORDER_TYPE_LIST)

/**
 * @brief Public order request handed to OrderBook::place_order.
 *
 * Required fields come first so designated initialisers stay terse:
 * @code
 * book.place_order({.id = 1, .side = Side::bid, .price = 100, .qty = 10});
 * @endcode
 * @c type and @c timestamp default.
 */
struct Order {
	order_id id;
	side side;
	price price;
	quantity qty;
	OrderType type = OrderType::GOOD_TILL_CANCELLED;
	
	std::uint64_t timestamp = 0;

	[[nodiscard]] bool is_buy() const noexcept;

	[[nodiscard]] bool has_quantity() const noexcept;

	void decrease_volume_by(quantity v) noexcept;

	bool operator==(const Order &) const noexcept = default;
};

} // namespace exchange::engine
