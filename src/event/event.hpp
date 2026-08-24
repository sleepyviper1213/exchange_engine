#pragma once

// Domain events (scaffold).
//
// The event-sourcing vocabulary the engine emits and persistence::event_store
// records - order accepted/cancelled, trade executed, level changed, etc.
// Not implemented yet - this header only fixes the module's shape and namespace.

namespace exchange::engine::event {

// TODO: define the domain event types.
// struct OrderAccepted { ... };
// struct TradeExecuted { ... };

} // namespace event
