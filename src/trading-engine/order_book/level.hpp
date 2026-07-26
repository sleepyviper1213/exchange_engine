#pragma once
#include "fwd.hpp"
#include "order.hpp"

#include <vector>

namespace exchange::engine {

/// @brief One price level: a price and the FIFO of orders resting at it.
///
/// Orders are held oldest-first (front() fills first). Kept as a plain vector
/// while the pool-backed intrusive list is set aside.
struct Level {
    Price price;
    std::vector<Order> orders;

    TRADING_ENGINE_EXPORT void add_order(const Order &order);

    /// @brief True when no orders rest at this level.
    [[nodiscard]] TRADING_ENGINE_EXPORT bool has_empty_orders() const noexcept;

    /// @brief Sum of the resting orders' volumes.
    [[nodiscard]] TRADING_ENGINE_EXPORT Volume total_volume() const noexcept;
};

} // namespace exchange::engine