#pragma once

#include "trading_engine_export.hpp"

#include <bit>
#include <cstddef>

namespace exchange::engine::execution {


template <std::size_t QueueCapacity = 1U << 14>
	requires (std::has_single_bit(QueueCapacity))
class engine_partition;

class TRADING_ENGINE_EXPORT book_manager;
class TRADING_ENGINE_EXPORT dispatcher;
class TRADING_ENGINE_EXPORT matching_engine;

} // namespace exchange::engine::execution