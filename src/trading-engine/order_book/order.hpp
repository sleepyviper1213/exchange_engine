#pragma once
#include "types.hpp"
#include "side.hpp"

namespace order_book {

/// @brief Time-in-force / execution policy for an incoming order.
/// matching-time policy
enum class OrderType {
    // MARKET,
    // LIMIT,
    // STOP,
    GOOD_TILL_CANCELED, ///< rest the unfilled remainder indefinitely
    FILL_OR_KILL,       ///< execute fully and immediately, or not at all
    IMMEDIATE_OR_CANCEL ///< execute what crosses now, drop the remainder
};

/**
 * @brief Public order request handed to OrderBook::place_order.
 *
 * Required fields come first so designated initializers stay terse:
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
    OrderType type = OrderType::GOOD_TILL_CANCELED;
    uint64_t timestamp{};

	bool is_buy() const noexcept;

	bool has_quantity() const noexcept;

	void decrease_volume_by(Volume volume) noexcept;

	bool operator==(const Order &) const noexcept = default;
};

} // namespace order_book
