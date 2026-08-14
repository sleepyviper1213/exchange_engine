#pragma once

#include <cstddef>

namespace exchange::engine::execution {
template <std::size_t QueueCapacity = 1U << 14>
class engine_partition;

class book_manager;
class dispatcher;
class matching_engine;
class order_manager;

struct order_handle;
struct order_record;

} // namespace exchange::engine::execution
