#pragma once
#include "core/types.hpp"
#include "core/util/enum_string.hpp"
#include "fwd.hpp"

namespace exchange::engine {

/**
 * @brief Single source of truth for @c OrderType's enumerators.
 *
 * Each entry is @c X(enumerator, "meaning"). The enum and its @c to_string
 * accessor both expand from this list via the shared X-macro helpers (see
 * @c core/util/enum_string.hpp), so the definition and its string form can
 * never drift apart — a stopgap for C++26 static reflection. Future
 * MARKET / LIMIT / STOP types are added here.
 */
#define ORDER_TYPE_LIST(X)                                                     \
	X(GOOD_TILL_CANCELLED, "rest the unfilled remainder indefinitely")         \
	X(FILL_OR_KILL, "execute fully and immediately, or not at all")            \
	X(IMMEDIATE_OR_CANCEL, "execute what crosses now, drop the remainder")

/// @brief Time-in-force / execution policy for an incoming order.
enum class OrderType { EXCHANGE_ENUM_VALUES(ORDER_TYPE_LIST) };

/// @brief The enumerator name of an @c OrderType, e.g. @c "FILL_OR_KILL"
///        (empty view if out of range).
EXCHANGE_ENUM_NAME(OrderType, to_string, ORDER_TYPE_LIST)

/**
 * @brief Public order request handed to OrderBook::place_order.
 *
 * Required fields come first so designated initialisers stay terse:
 * @code
 * book.place_order({.id = 1, .side = Side::BID, .price = 100, .volume = 10});
 * @endcode
 * @c type and @c date_time default.
 */
struct Order {
	OrderId id;
	Side side;
	Price price;
	Volume volume;
	OrderType type = OrderType::GOOD_TILL_CANCELLED;
	uint64_t timestamp;

	[[nodiscard]] bool is_buy() const noexcept;

	[[nodiscard]] bool has_quantity() const noexcept;

	void decrease_volume_by(Volume volume) noexcept;

	bool operator==(const Order &) const noexcept = default;
};

} // namespace exchange::engine
