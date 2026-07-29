#pragma once

#include "trading_engine_export.hpp"

#include <cstddef>
#include <cstdint>

namespace exchange::engine {

enum class OrderType : std::uint8_t;

struct TRADING_ENGINE_EXPORT Order;

struct Level;
struct TRADING_ENGINE_EXPORT Trade;

class order_book;

} // namespace exchange::engine
