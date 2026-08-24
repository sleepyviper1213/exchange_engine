#pragma once
// trade execution event (scaffold).
//
// The event-sourcing record emitted for each fill produced by matching, for
// persistence::event_store and downstream consumers. Distinct from
// order_book::trade (the in-memory matching output). Not implemented yet; this
// header only fixes the module's shape and namespace.

namespace exchange::engine::event::trade {

// TODO: define the trade event type.
// struct TradeExecuted { ... };

} // namespace exchange::engine::event::trade
