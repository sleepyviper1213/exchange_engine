#pragma once

#include <cstddef>

namespace order_book {

// Scoped enums: the underlying type must match the definition exactly.
enum class Side : bool;
enum class OrderType;

struct Order;
struct Level;
struct Trade;

class book_side;
class OrderBook;
class OrderList;
class RestingOrder;

} // namespace order_book

namespace event {

struct Command;
struct LevelChange;

} // namespace event

namespace execution {

// The default argument lives here (declared once) so that including the
// definition header, which now omits it, still sees it.
template <std::size_t QueueCapacity = 1U << 14>
class MatchingEngine;

} // namespace execution
