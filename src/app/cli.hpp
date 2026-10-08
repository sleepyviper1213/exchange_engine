#pragma once

#include "core/metrics/fwd.hpp"

namespace CLI {
class App;
}

// Registers the exchange_tool subcommands on a CLI11 app. Each function adds
// one subcommand with its options and wires its callback to set @p rc. One
// registrar per file under app/cli/, next to the option struct it binds; the
// command drivers live under app/commands/. main() calls these, so the tool's
// shape (which commands exist) is visible at the entry point.
//
// --- the lifetime rule every registrar obeys --------------------------------
//
// Each registrar binds CLI11 options to storage that must outlive the call, so
// every one holds its option variables in a `<command>_options` on the heap.
// This is not a style choice and getting it wrong is not a warning:
// `add_option` keeps a *pointer* to the variable and the callback captures it,
// while parsing runs later, back in main() - so a plain local is written to and
// read from long after its scope has ended. `add_snapshot`, `add_capture` and
// `add_replay` once used locals and segfaulted on any invocation that reached
// their driver.
//
// The shared_ptr copy captured by value into each subcommand's callback is what
// keeps the state alive: the callback belongs to the subcommand, the subcommand
// to the App, and the App outlives CLI11_PARSE. Function statics did the same
// job and are what this replaced - they carry static storage duration, which
// MISRA 6-7-1 forbids, and they made the storage per-process rather than per
// registration, so building the CLI twice would have shared one set.
//
// The consequence to respect: a callback must reach the state through `state->`
// and capture the shared_ptr BY VALUE. Capturing it, or any reference into it,
// by reference reintroduces exactly the dangling read the statics fixed.
namespace exchange::app {

void add_snapshot(CLI::App &app, int &rc);
void add_capture(CLI::App &app, int &rc);
void add_live(CLI::App &app, int &rc);
void add_replay(CLI::App &app, int &rc);
void add_trades(CLI::App &app, int &rc);
void add_account(CLI::App &app, int &rc);
void add_watchdog(CLI::App &app, int &rc);
void add_backtest(CLI::App &app, int &rc);
void add_recover(CLI::App &app, int &rc);
void add_demo(CLI::App &app, int &rc,
			  const core::metrics::settings &metrics_settings);
void add_serve(CLI::App &app, int &rc,
			   const core::metrics::settings &metrics_settings);

} // namespace exchange::app
