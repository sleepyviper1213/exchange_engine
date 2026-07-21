#pragma once
#include "order.hpp"
#include "types.hpp"

#include <vector>

namespace core::engine {

/// @brief One price level: a price and the FIFO of orders resting at it.
///
/// Orders are held oldest-first (front() fills first). Kept as a plain vector
/// while the pool-backed intrusive list is set aside.
struct Level {
    Price price;
    std::vector<Order> orders;

    void add_order(const Order &order);

    /// @brief True when no orders rest at this level.
    [[nodiscard]] bool has_empty_orders() const noexcept;

    /// @brief Sum of the resting orders' volumes.
    [[nodiscard]] Volume total_volume() const noexcept;
};

} // namespace core::engine