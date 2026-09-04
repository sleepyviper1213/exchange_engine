#pragma once

#include "orders_export.hpp"

#include <cstdint>

// The order module's vocabulary lives in `orders`, plural, because the module's
// principal type is `order`, singular: a namespace and a class of the same name
// in the same scope is not a thing C++ lets you have, and the lower-case
// convention here leaves no other way to tell them apart.
namespace exchange::engine::orders {

struct ORDERS_EXPORT order;

struct stop_order;
struct LimitOrder;
struct cache_optimised_level;

enum class order_type : std::uint8_t;
enum class time_in_force_instruction : std::uint8_t;
} // namespace exchange::engine::orders
