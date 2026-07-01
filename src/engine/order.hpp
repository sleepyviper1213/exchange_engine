#pragma once
#include <chrono>
#include <cstdint>
#include <type_traits>
#include "side.hpp"

/// @brief Time-in-force / execution policy for an incoming order.
enum class OrderType {
    // MARKET,
    // LIMIT,
    // STOP,
    GOOD_TILL_CANCELED, ///< rest the unfilled remainder indefinitely
    FILL_OR_KILL,       ///< execute fully and immediately, or not at all
    IMMEDIATE_OR_CANCEL ///< execute what crosses now, drop the remainder
};

using Price = std::uint64_t;
using Volume = std::int64_t;
using OrderId = std::uint64_t;

static_assert(!std::is_floating_point_v<Price>, "Price must not be floating point");
static_assert(std::is_unsigned_v<Price>, "Price must be unsigned");

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
    // std::chrono::system_clock::time_point date_time{}; ///< receive timestamp

    bool operator==(const Order &) const noexcept = default;
};

/**
 * @brief One execution produced by matching.
 * @note Trades always print at the resting (passive) order's price.
 */
struct Trade {
    OrderId aggressor; ///< id of the incoming, aggressing order
    OrderId resting;   ///< id of the passive order that was hit
    Price price;       ///< execution price (the resting order's price)
    Volume volume;     ///< executed quantity
};
