#pragma once

#include <cstddef>
#include <cstdint>

namespace exchange::engine::execution {
template <std::size_t QueueCapacity = 1U << 14>
class engine_partition;

enum class record_flag : std::uint8_t;

class book_manager;
class dispatcher;
class matching_engine;
class order_manager;

struct order_handle;
struct order_record;

} // namespace exchange::engine::execution
