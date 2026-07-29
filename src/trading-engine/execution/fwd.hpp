#pragma once

#include "trading_engine_export.hpp"

#include <bit>
#include <cstddef>

namespace exchange::engine::execution {


template <std::size_t QueueCapacity = 1U << 14>
	requires (std::has_single_bit(QueueCapacity))
class MatchingEngine;

// TODO: forward-declare the execution scaffold types (BookManager, Dispatcher,
// EnginePartition) once they gain real definitions.

} // namespace exchange::engine::execution