#pragma once
// Forward declarations for the strategy submodule.
//
// A strategy generates commands. It never touches an order_book, never sees a
// queue, and never learns which thread it runs on — it is handed what the engine
// published and writes commands into a buffer somebody else owns.

#include "trading-engine/order_book/fwd.hpp" // IWYU pragma: export
#include "trading-engine/orders/fwd.hpp"     // IWYU pragma: export

#include <cstddef>

namespace exchange::strategy {

class command_writer;

template <std::size_t Capacity>
class command_batch;

// strategy_engine is deliberately absent. Its parameters are constrained, and a
// declaration that omits the constraints is a *different* template rather than a
// forward reference to the same one — so declaring it here would need concepts.hpp,
// which needs command_writer.hpp, which is the whole weight this header exists to
// avoid. Name the host and you are including engine.hpp anyway.

template <std::size_t MaxWorking>
class iceberg;

template <std::size_t MaxArmed>
class stop;

} // namespace exchange::strategy
