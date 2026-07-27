#pragma once
// Engine shut-down event (scaffold).
//
// Emitted when the matching engine drains and stops, so persistence and
// downstream consumers can close out a session cleanly. Not implemented yet;
// this header only fixes the module's shape and namespace.

namespace exchange::engine::event::lifecycle {

// TODO: define the shut-down event type.
// struct Shutdown { ... };

} // namespace exchange::engine::event::lifecycle
