// Option wiring for `exchange_tool live`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/live.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/live.hpp"

#include <CLI/CLI.hpp>

#include <memory>
#include <string>

namespace exchange::app {

namespace {
struct live_options {
	std::string symbol;
	std::string speed  = "100ms";
	int seconds        = 30;
	int limit          = 100;
	int price_decimals = 2;
	int qty_decimals   = 2;
	int depth          = 10;
	bool insecure_tls  = false;
};
} // namespace

void add_live(CLI::App &app, int &rc) {
	auto *live = app.add_subcommand(
		"live",
		"Track a Binance book live: diff stream plus on-demand snapshots");

	auto state = std::make_shared<live_options>();
	live->add_option("symbol", state->symbol, "Binance symbol")->required();
	live->add_option("--seconds",
					 state->seconds,
					 "How long to track; 0 runs until the stream ends")
		->capture_default_str();
	live->add_option("--speed", state->speed, "Update cadence")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	live->add_flag("--insecure-tls",
				   state->insecure_tls,
				   "Do not verify the feed's TLS certificate. For a host with "
				   "no CA bundle");
	live->add_option(
			"--limit",
			state->limit,
			"REST snapshot depth per side. Above the replica's retained "
			"depth (l2_book keeps 128 a side) the surplus is fetched, "
			"parsed and then dropped")
		->capture_default_str();
	live->add_option("--price-decimals",
					 state->price_decimals,
					 "Tick precision")
		->capture_default_str();
	live->add_option("--qty-decimals", state->qty_decimals, "Step precision")
		->capture_default_str();
	live->add_option("--depth",
					 state->depth,
					 "Ladder rows per side to print at the end (0 = all, which "
					 "is up to the 128 the replica retains)")
		->capture_default_str();
	live->callback([&rc, state] {
		rc = cmd_live(state->symbol,
					  state->seconds,
					  state->speed,
					  state->limit,
					  state->price_decimals,
					  state->qty_decimals,
					  state->depth,
					  state->insecure_tls);
	});
}

} // namespace exchange::app
