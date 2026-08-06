#pragma once

#include <bit>
#include <cstddef>

namespace exchange::engine::execution {

template <std::size_t QueueCapacity = 1U << 14>
	requires (std::has_single_bit(QueueCapacity))
class engine_partition;

class book_manager;
class dispatcher;
class matching_engine;

} // namespace exchange::engine::execution