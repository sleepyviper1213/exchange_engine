#pragma once
// Event-log replay (scaffold).
//
// Reads events back from the persistence::event_store and re-applies them to
// rebuild book state after a restart. Not implemented yet; this header only
// fixes the module's shape and namespace.

namespace exchange::core::persistence {

// TODO: implement the event-log replay driver.

} // namespace exchange::core::persistence
