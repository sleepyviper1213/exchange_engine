// Usage. Settings come from built-in defaults, then ./exchange_tool.ini (or
// whatever --config names), then the command line; each beats the one before.
// See configuration.hpp.
//
//   exchange_tool [--config FILE.ini] [--log-*] <command> ...
//
//   exchange_tool snapshot SYMBOL [--limit N] [--price-decimals N]
//   [--qty-decimals N] exchange_tool snapshot --file <depth.json>
//   [--price-decimals N] [--qty-decimals N] exchange_tool capture  SYMBOL
//   OUTFILE [--seconds N] [--speed 100ms|1000ms] exchange_tool replay
//   FILE.jsonl [--snapshot seed.json] [--price-decimals N] [--qty-decimals N]
//   exchange_tool demo     [num_orders]

#include "cli.hpp"
#include "configuration.hpp"
#include "core/logging.hpp"

#include <CLI/CLI.hpp>
#include <internal_use_only/config.hpp>

#include <cstdlib>
#include <optional>
#include <string>

int main(int argc, char **argv) {
	using namespace exchange::app;
	namespace logging = exchange::core::logging;
	namespace metrics = exchange::core::metrics;

	CLI::App app{"exchange_tool -- order-book market-data & engine CLI"};

	logging::settings log_settings;
	metrics::settings metrics_settings;
	add_configuration(app, log_settings, metrics_settings);
	app.set_version_flag("--version",
						 std::string{exchange::cmake::project_version});
	app.require_subcommand(1);

	logging::guard log{log_settings};

	int rc = EXIT_SUCCESS;
	add_snapshot(app, rc); // fetch/load a depth snapshot → book → top of book
	add_capture(app, rc);  // stream a diff-depth WebSocket to a JSONL file
	add_replay(app, rc);   // replay a JSONL capture through an OrderBook
	add_demo(app, rc, metrics_settings); // run the MatchingEngine end-to-end

	CLI11_PARSE(app, argc, argv);
	return rc;
}
