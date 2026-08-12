#pragma once

#include "core/metrics/settings.hpp"

#include <CLI/CLI.hpp>

// Registers the exchange_tool subcommands on a CLI11 app. Each function adds one
// subcommand with its options and wires its callback to set @p rc. The option
// wiring and the command drivers live in cli.cpp; main() calls these so the
// tool's shape (which commands exist) is visible at the entry point.
namespace exchange::app {

void add_snapshot(CLI::App &app, int &rc);
void add_capture(CLI::App &app, int &rc);
void add_replay(CLI::App &app, int &rc);
void add_demo(CLI::App &app, int &rc,
			 const core::metrics::settings &metrics_settings);

} // namespace cli
