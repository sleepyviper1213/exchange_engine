#pragma once

// The order vocabulary the book is built on lives one module over, in
// exchange::engine::orders - plural, because its principal type is `order`.
#include "orders/fwd.hpp" // IWYU pragma: export

#include <cstdint>

namespace exchange::engine {


enum class allocation_policy : std::uint8_t;

struct price_level;
struct queue_position;
struct sweep_estimate;
class order_book;
struct resting_view;
} // namespace exchange::engine
