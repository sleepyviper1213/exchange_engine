// Option wiring for `exchange_tool recover`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/recover.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/recover.hpp"

#include <CLI/CLI.hpp>

#include <memory>

namespace exchange::app {

namespace {
struct recover_options {
	recover_settings settings;
};
} // namespace

void add_recover(CLI::App &app, int &rc) {
	auto *rec = app.add_subcommand(
		"recover",
		"Recover a journalled store, add resting flow, and checkpoint");

	auto state = std::make_shared<recover_options>();
	rec->add_option("--store",
					state->settings.store,
					"Directory holding the journal")
		->capture_default_str();
	rec->add_option("--orders",
					state->settings.orders,
					"Resting orders to add after recovering")
		->capture_default_str();
	rec->add_flag("--checkpoint",
				  state->settings.checkpoint,
				  "Snapshot the books and commit a manifest before stopping");
	rec->add_flag("--recover-only",
				  state->settings.recover_only,
				  "Recover and report without adding any flow");
	rec->callback([&rc, state] { rc = cmd_recover(state->settings); });
}

} // namespace exchange::app
