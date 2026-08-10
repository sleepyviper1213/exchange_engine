#pragma once

// The order vocabulary the book is built on lives one module over, in
// exchange::engine::orders — plural, because its principal type is `order`.
#include "trading-engine/orders/fwd.hpp" // IWYU pragma: export
#include "trading_engine_export.hpp"

#include <cstdint>

namespace exchange::engine {


enum class OrderStatus : std::uint8_t;
enum class OutcomeType : std::uint8_t;
enum class reject_reason : std::uint8_t;

struct price_level;
struct TRADING_ENGINE_EXPORT trade;

// Declared without the dll interface, like order_book below: both export their
// members individually, and MSVC rejects a member marked dllexport inside a
// class that is already dllexport (C2487). Whole-type export is for the plain
// aggregates above, which have no exported members of their own.
struct order_outcome;
class order_state;
class order_book;

} // namespace exchange::engine
