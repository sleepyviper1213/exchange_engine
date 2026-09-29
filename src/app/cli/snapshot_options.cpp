// Option wiring for `exchange_tool snapshot`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/snapshot.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/snapshot.hpp"

#include <CLI/CLI.hpp>

#include <memory>
#include <string>

namespace exchange::app {

namespace {
struct snapshot_options {
	std::string symbol;
	std::string file;
	int limit          = 100;
	int price_decimals = 2;
	int qty_decimals   = 2;
};
} // namespace

void add_snapshot(CLI::App &app, int &rc) {
	auto *snap = app.add_subcommand(
		"snapshot",
		"Fetch (or load) a Binance depth snapshot and print top of book");

	auto state = std::make_shared<snapshot_options>();
	snap->add_option("symbol", state->symbol, "Binance symbol, e.g. SOLUSDT");
	snap->add_option("--file",
					 state->file,
					 "Load a saved depth JSON or fetching live");
	snap->add_option("--limit", state->limit, "REST depth limit")
		->capture_default_str();
	snap->add_option("--price-decimals",
					 state->price_decimals,
					 "Tick precision")
		->capture_default_str();
	snap->add_option("--qty-decimals", state->qty_decimals, "Step precision")
		->capture_default_str();
	snap->callback([&rc, state] {
		if (state->symbol.empty() && state->file.empty())
			throw CLI::ValidationError("snapshot", "provide SYMBOL or --file");
		rc = cmd_snapshot(state->symbol,
						  state->file,
						  state->limit,
						  state->price_decimals,
						  state->qty_decimals);
	});
}

} // namespace exchange::app
