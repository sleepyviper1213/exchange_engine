#pragma once

// The order vocabulary the book is built on lives one module over, in
// exchange::engine::orders - plural, because its principal type is `order`.
#include "orders/fwd.hpp" // IWYU pragma: export

#include <cstdint>

namespace exchange::engine {


enum class OrderStatus : std::uint8_t;
enum class OutcomeType : std::uint8_t;
enum class reject_reason : std::uint8_t;
enum class allocation_policy : std::uint8_t;

struct price_level;
struct trade;
struct order_outcome;
struct queue_position;
struct sweep_estimate;
class order_state;
class order_book;
struct resting_view;
struct sweep_estimate;
} // namespace exchange::engine
