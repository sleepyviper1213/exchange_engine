#include "cli.hpp"

#include "commands.hpp"
#include "core/metrics/settings.hpp"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <string>

namespace exchange::app {

// Each registrar binds CLI11 options to storage that must outlive the call, so
// every option variable below is function static. This is not a style choice and
// getting it wrong is not a warning: `add_option` keeps a *pointer* to the
// variable and the callback captures it by reference, while parsing runs later,
// back in main() - so a plain local is written to and read from long after its
// scope has ended. `add_snapshot`, `add_capture` and `add_replay` used locals and
// segfaulted on any invocation that reached their driver; `add_demo` and
// `add_backtest` were already static, which is why those two worked.
//
// The CLI is built and parsed exactly once, so the shared storage is safe - and
// a function-local static is initialised once and thread-safely besides.
void add_snapshot(CLI::App &app, int &rc) {
	auto *snap = app.add_subcommand(
		"snapshot",
		"Fetch (or load) a Binance depth snapshot and print top of book");
	static std::string symbol, file;
	static int limit          = 100;
	static int price_decimals = 2;
	static int qty_decimals   = 2;
	snap->add_option("symbol", symbol, "Binance symbol, e.g. SOLUSDT");
	snap->add_option("--file",
					 file,
					 "Load a saved depth JSON or fetching live");
	snap->add_option("--limit", limit, "REST depth limit")
		->capture_default_str();
	snap->add_option("--price-decimals", price_decimals, "Tick precision")
		->capture_default_str();
	snap->add_option("--qty-decimals", qty_decimals, "Step precision")
		->capture_default_str();
	snap->callback([&] {
		if (symbol.empty() && file.empty())
			throw CLI::ValidationError("snapshot", "provide SYMBOL or --file");
		rc = cmd_snapshot(symbol, file, limit, price_decimals, qty_decimals);
	});
}

void add_capture(CLI::App &app, int &rc) {
	auto *cap = app.add_subcommand(
		"capture",
		"Stream a Binance diff-depth WebSocket to a JSONL file");
	static std::string symbol;
	static std::string outfile;
	static std::string speed = "100ms";
	static int seconds       = 30;
	cap->add_option("symbol", symbol, "Binance symbol")->required();
	cap->add_option("outfile", outfile, "Destination JSONL file")->required();
	cap->add_option("--seconds", seconds, "Recording duration (seconds)")
		->capture_default_str();
	cap->add_option("--speed", speed, "Update cadence")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	cap->callback([&] { rc = cmd_capture(symbol, outfile, seconds, speed); });
}

void add_demo(CLI::App &app, int &rc,
			  const core::metrics::settings &metrics_settings) {
	auto *demo = app.add_subcommand(
		"demo",
		"Run the MatchingEngine end-to-end over the SPSC queue");
	static std::uint64_t num_orders = 2'000'000;
	demo->add_option("num_orders", num_orders, "Synthetic orders to submit")
		->capture_default_str();
	demo->callback([&rc, &metrics_settings] {
		rc = cmd_demo(num_orders, metrics_settings);
	});
}

void add_replay(CLI::App &app, int &rc) {
	auto *replay = app.add_subcommand(
		"replay",
		"Replay a JSONL diff-depth capture through an OrderBook");
	static std::string file;
	static std::string snapshot;
	static int price_decimals = 2;
	static int qty_decimals   = 2;
	replay->add_option("file", file, "JSONL capture of depthUpdate frames")
		->required()
		->check(CLI::ExistingFile);
	replay
		->add_option("--snapshot",
					 snapshot,
					 "Seed the book from a saved REST depth JSON")
		->check(CLI::ExistingFile);
	replay->add_option("--price-decimals", price_decimals, "Tick precision")
		->capture_default_str();
	replay->add_option("--qty-decimals", qty_decimals, "Step precision")
		->capture_default_str();
	replay->callback(
		[&] { rc = cmd_replay(file, snapshot, price_decimals, qty_decimals); });
}

void add_recover(CLI::App &app, int &rc) {
	auto *rec = app.add_subcommand(
		"recover",
		"Recover a journalled store, add resting flow, and checkpoint");
	// One static aggregate keeps every option's storage alive past this call, the
	// same way add_backtest does. @see the note above add_snapshot.
	static recover_settings settings;
	rec->add_option("--store", settings.store, "Directory holding the journal")
		->capture_default_str();
	rec->add_option("--orders",
					settings.orders,
					"Resting orders to add after recovering")
		->capture_default_str();
	rec->add_flag("--checkpoint",
				  settings.checkpoint,
				  "Snapshot the books and commit a manifest before stopping");
	rec->add_flag("--recover-only",
				  settings.recover_only,
				  "Recover and report without adding any flow");
	rec->callback([&rc] { rc = cmd_recover(settings); });
}

void add_backtest(CLI::App &app, int &rc) {
	auto *bt = app.add_subcommand(
		"backtest",
		"Run a strategy against a recorded capture through the whole engine");
	// Static for the same reason the other registrars' locals are not: this one
	// is a single aggregate, so one static keeps every option's storage alive
	// until CLI11 parses, instead of a dozen.
	static backtest_settings settings;

	bt->add_option("file", settings.file, "JSONL capture of depthUpdate frames")
		->required()
		->check(CLI::ExistingFile);
	bt->add_option("--snapshot",
				   settings.snapshot,
				   "REST depth JSON seeding the replica. Required: without a "
				   "seed the book only holds prices the capture happened to "
				   "touch, and nothing meaningful can fill against it")
		->required()
		->check(CLI::ExistingFile);
	bt->add_option("--symbol", settings.symbol, "Display name for the listing")
		->capture_default_str();
	bt->add_option("--price-decimals", settings.price_decimals, "Price scale")
		->capture_default_str();
	bt->add_option("--qty-decimals", settings.qty_decimals, "Quantity scale")
		->capture_default_str();
	bt->add_option(
		  "--tick",
		  settings.tick,
		  "Price increment, e.g. 0.01. Must be exact at --price-decimals")
		->capture_default_str();
	bt->add_option("--lot",
				   settings.lot,
				   "Size increment, e.g. 0.01. Must be exact at --qty-decimals")
		->capture_default_str();
	bt->add_option("--events",
				   settings.events,
				   "Stop after this many frames (0 = the whole file)")
		->capture_default_str();
	bt->add_option("--improve",
				   settings.improve_ticks,
				   "Ticks inside the venue's touch to quote. Below 1 the quote "
				   "can never be traded through and will never fill passively")
		->capture_default_str();
	bt->add_option("--lots", settings.lots, "Quote size, in lots")
		->capture_default_str();
	bt->add_option(
		  "--requote-ms",
		  settings.requote_ms,
		  "Market time a quote is left standing before it is replaced. "
		  "Zero requotes on every frame, which is also why zero fills "
		  "nothing: the quoter steps out of the way before the market "
		  "ever reaches it, and being slower is what creates exposure")
		->capture_default_str();
	bt->add_option("--max-position",
				   settings.max_position,
				   "Risk limit on absolute net position, in lots (0 = open)")
		->capture_default_str();
	bt->add_flag("--fill-on-lock",
				 settings.fill_on_lock,
				 "Fill a resting order when the venue quotes *at* its price, "
				 "not only when it trades through. Strictly more optimistic");
	bt->add_flag("--no-quote{false}",
				 settings.quote,
				 "Replay with no order flow - exercises the harness, not a "
				 "strategy; every fill counter must come back zero");

	bt->callback([&rc] { rc = cmd_backtest(settings); });
}

} // namespace exchange::app
