#pragma once

#include "core/types.hpp"
#include "trading_engine_export.hpp"

#include <cstddef>

namespace exchange::engine {

enum class OrderType;

struct TRADING_ENGINE_EXPORT Order;
struct TRADING_ENGINE_EXPORT Level;
struct TRADING_ENGINE_EXPORT Trade;

class TRADING_ENGINE_EXPORT order_book;

// namespace detail {
// class book_side;
// class OrderList;
// class TRADING_ENGINE_AUTOTEST_EXPORT RestingOrder;
// } // namespace detail
} // namespace exchange::engine
