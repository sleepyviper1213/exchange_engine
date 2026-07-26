#pragma once

#include "core/types.hpp"
#include "trading_engine_export.hpp"

namespace exchange::engine::execution {

// The default argument lives here (declared once) so that including the
// definition header, which now omits it, still sees it.
template <std::size_t QueueCapacity = 1U << 14>
class TRADING_ENGINE_EXPORT MatchingEngine;
} // namespace exchange::engine