#pragma once

#include "core/types.hpp"
#include "trading_engine_export.hpp"

#include <cstddef>

namespace exchange::engine {

enum class OrderType;

struct TRADING_ENGINE_EXPORT Order;

struct Level;
struct TRADING_ENGINE_EXPORT Trade;

class order_book;

} // namespace exchange::engine
