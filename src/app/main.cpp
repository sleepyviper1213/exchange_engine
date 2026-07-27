//   exchange_tool snapshot SYMBOL [--limit N] [--price-decimals N]
//   [--qty-decimals N] exchange_tool snapshot --file <depth.json>
//   [--price-decimals N] [--qty-decimals N] exchange_tool capture  SYMBOL
//   OUTFILE [--seconds N] [--speed 100ms|1000ms] exchange_tool replay
//   FILE.jsonl [--snapshot seed.json] [--price-decimals N] [--qty-decimals N]
//   exchange_tool demo     [num_orders]

#include "cli.hpp"
#include "configuration.hpp"
#include "logger.hpp"

#include <CLI/CLI.hpp>
#include <internal_use_only/config.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
	using namespace exchange::app;
	const auto config = Configuration::from_env();
	init_logging(config);

	CLI::App app{"exchange_tool -- order-book market-data & engine CLI"};
	app.set_version_flag("--version",
	                     std::string{exchange::cmake::project_version});
	app.require_subcommand(1);
	int rc = EXIT_SUCCESS;

	add_snapshot(app, rc); // fetch/load a depth snapshot → book → top of book
	add_capture(app, rc); // stream a diff-depth WebSocket to a JSONL file
	add_replay(app, rc); // replay a JSONL capture through an OrderBook
	add_demo(app, rc); // run the MatchingEngine end-to-end

	CLI11_PARSE(app, argc, argv);
	return rc;
}