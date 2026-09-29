// Option wiring for `exchange_tool replay`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/replay.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/replay.hpp"

#include <CLI/CLI.hpp>

#include <memory>
#include <string>

namespace exchange::app {

namespace {
struct replay_options {
	std::string file;
	std::string snapshot;
	int price_decimals = 2;
	int qty_decimals   = 2;
};
} // namespace

void add_replay(CLI::App &app, int &rc) {
	auto *replay = app.add_subcommand(
		"replay",
		"Replay a JSONL diff-depth capture through an OrderBook");

	auto state = std::make_shared<replay_options>();
	replay
		->add_option("file", state->file, "JSONL capture of depthUpdate frames")
		->required()
		->check(CLI::ExistingFile);
	replay
		->add_option("--snapshot",
					 state->snapshot,
					 "Seed the book from a saved REST depth JSON")
		->check(CLI::ExistingFile);
	replay
		->add_option("--price-decimals",
					 state->price_decimals,
					 "Tick precision")
		->capture_default_str();
	replay->add_option("--qty-decimals", state->qty_decimals, "Step precision")
		->capture_default_str();
	replay->callback([&rc, state] {
		rc = cmd_replay(state->file,
						state->snapshot,
						state->price_decimals,
						state->qty_decimals);
	});
}

} // namespace exchange::app
