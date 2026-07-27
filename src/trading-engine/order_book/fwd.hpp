#pragma once

#include "core/types.hpp"
#include "trading_engine_export.hpp"

#include <cstddef>

namespace exchange::engine {

enum class OrderType;

struct TRADING_ENGINE_EXPORT Order;
// Level and order_book export their members individually (see level.hpp /
// order_book.hpp), so they must NOT be class-level exported here — a whole-class
// dll-interface makes MSVC reject the per-member export macros (C2487). Order
// and Trade have no per-member exports and stay class-level exported.
struct Level;
struct TRADING_ENGINE_EXPORT Trade;

class order_book;

namespace detail {
class book_side;
class OrderList;
class TRADING_ENGINE_AUTOTEST_EXPORT RestingOrder;
} // namespace detail
} // namespace exchange::engine
