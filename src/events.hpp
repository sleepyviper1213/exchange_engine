#pragma once
// Umbrella header for the trading-engine event submodule: the event-sourcing
// vocabulary the engine emits and the command inputs the matching engine
// consumes. Prefer event/fwd.hpp when a declaration suffices.
// IWYU pragma: begin_exports
#include "event/command.hpp"
#include "event/engine_event.hpp"
#include "event/event.hpp"
#include "event/journal_record.hpp"
#include "event/event_channel.hpp"
#include "event/event_dispatcher.hpp"
#include "event/lifecycle/lifecycle.hpp"
#include "event/trade/trade.hpp"
// IWYU pragma: end_exports
