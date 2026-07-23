#pragma once
#include "order_book_export.h" // ORDER_BOOK_EXPORT (generated)

#include <cstdint>
#include <type_traits>

namespace order_book {

/// @brief Fixed-point price in ticks; integral so equality/ordering are exact.
using Price = std::uint64_t;
/// @brief Signed quantity; the L2 diff feed expresses reductions as negatives.
using Volume = std::int64_t;
/// @brief Stable identifier for a client order.
using OrderId = std::uint64_t;

static_assert(!std::is_floating_point_v<Price>, "Price must not be floating point");
static_assert(std::is_unsigned_v<Price>, "Price must be unsigned");

} // namespace order_book
