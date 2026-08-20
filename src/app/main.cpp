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
//   exchange_tool backtest FILE.jsonl --snapshot seed.json [--tick 0.01]
//   [--lot 0.01] [--improve N] [--lots N] [--fill-on-lock] [--no-quote]
//   exchange_tool demo     [num_orders]

#include "cli.hpp"
#include "configuration.hpp"
#include "core/logging.hpp"
#include "core/metrics/settings.hpp"

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

	// Deferred, not constructed here: log_settings still holds built-in
	// defaults at this point, and CLI11_PARSE below is what fills it in from
	// the config file and the command line - a guard built now would freeze
	// those defaults in before they existed, and every --log-* flag would be
	// silently ignored.
	//
	// parse_complete_callback() rather than callback() + immediate_callback():
	// App::callback() files a callback into parse_complete_callback_ only when
	// immediate_callback_ is already true, and setting immediate_callback_ on
	// @p app before add_demo/add_snapshot/... run means every subcommand
	// *inherits* it at construction - which makes CLI11 dispatch a
	// subcommand's own callback the moment its tokens finish parsing, ahead of
	// @p app's own end-of-parse run_callback() cascade. That fired cmd_demo
	// before this lambda ever ran. Calling parse_complete_callback() directly
	// sets @p app's slot without touching immediate_callback_ anywhere, so
	// every subcommand still resolves through the normal cascade - and that
	// cascade runs @p app's own parse_complete_callback_ before any
	// subcommand's, which is the ordering this needs.
	std::optional<logging::guard> log;
	app.parse_complete_callback([&] { log.emplace(log_settings); });

	int rc = EXIT_SUCCESS;
	add_snapshot(app, rc); // fetch/load a depth snapshot → book → top of book
	add_capture(app, rc);  // stream a diff-depth WebSocket to a JSONL file
	add_live(app, rc);     // track the venue's book live off the socket
	add_replay(app, rc);   // replay a JSONL capture through an OrderBook
	add_backtest(app, rc); // run the same capture through the whole engine
	add_recover(app, rc);  // recover a journalled store, add flow, checkpoint
	add_demo(app, rc, metrics_settings); // run the MatchingEngine end-to-end
	// The live path, and the only command that stays up: a venue's feed through
	// the depth bridge, a strategy through the risk gate, a matching engine on
	// its own thread, and the events routed back. @see commands/serve.hpp
	add_serve(app, rc, metrics_settings);

	CLI11_PARSE(app, argc, argv);
	return rc;
}
