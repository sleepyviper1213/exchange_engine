#pragma once

#include "orders_export.hpp"

#include <cstdint>

// The order module's vocabulary lives in `orders`, plural, because the module's
// principal type is `order`, singular: a namespace and a class of the same name
// in the same scope is not a thing C++ lets you have, and the lower-case
// convention here leaves no other way to tell them apart.
namespace exchange::engine::orders {

struct ORDERS_EXPORT order;
struct amendment;

struct stop_order;
struct LimitOrder;
struct cache_optimised_level;

enum class order_type : std::uint8_t;
enum class time_in_force_instruction : std::uint8_t;
} // namespace exchange::engine::orders

// What the engine reports having done to an order. Declared here rather than
// with the book that produces them, so the event vocabulary above can name a
// trade or an outcome without linking the book.
namespace exchange::engine {

enum class OrderStatus : std::uint8_t;
enum class OutcomeType : std::uint8_t;
enum class reject_reason : std::uint8_t;

struct trade;
struct order_outcome;
class order_state;
} // namespace exchange::engine
