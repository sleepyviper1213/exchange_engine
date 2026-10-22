#pragma once

// Declarations for the offline backtest harness.
//
// A submodule of strategy/ because it exists to answer a question about a
// strategy - "what would this have done" - and because it is the only consumer
// of the strategy hooks that is allowed to be slow. Nothing on a live path
// includes it: the simulated venue it runs against - the depth bridge, the fill
// model, the wire - is `simulation/`, which `session/` shares without naming
// the harness. @see simulation/fwd.hpp

namespace exchange::strategy::backtest {

struct report;
struct report_summary;

struct session_options;
class session;

} // namespace exchange::strategy::backtest
