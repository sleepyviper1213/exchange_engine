#pragma once

#include "trading_engine_export.hpp"

#include <cstddef>
#include <cstdint>

namespace exchange::engine {

enum class order_type : std::uint8_t;
enum class time_in_force_instruction : std::uint8_t;
enum class OrderStatus : std::uint8_t;
enum class OutcomeType : std::uint8_t;
enum class reject_reason : std::uint8_t;

struct TRADING_ENGINE_EXPORT order;

struct price_Level;
struct TRADING_ENGINE_EXPORT Trade;

// Declared without the dll interface, like order_book below: both export their
// members individually, and MSVC rejects a member marked dllexport inside a
// class that is already dllexport (C2487). Whole-type export is for the plain
// aggregates above, which have no exported members of their own.
struct OrderOutcome;
class order_state;
class order_book;

} // namespace exchange::engine
