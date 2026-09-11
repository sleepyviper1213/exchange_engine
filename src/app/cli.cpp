#include "cli.hpp"

#include "commands.hpp"
#include "core/metrics/settings.hpp"
#include "credentials_option.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>

namespace exchange::app {

// Each registrar binds CLI11 options to storage that must outlive the call, so
// every option variable below is function static. This is not a style choice
// and getting it wrong is not a warning: `add_option` keeps a *pointer* to the
// variable and the callback captures it by reference, while parsing runs later,
// back in main() - so a plain local is written to and read from long after its
// scope has ended. `add_snapshot`, `add_capture` and `add_replay` used locals
// and segfaulted on any invocation that reached their driver; `add_demo` and
// `add_backtest` were already static, which is why those two worked.
//
// The CLI is built and parsed exactly once, so the shared storage is safe - and
// a function-local static is initialised once and thread-safely besides.
void add_snapshot(CLI::App &app, int &rc) {
	auto *snap = app.add_subcommand(
		"snapshot",
		"Fetch (or load) a Binance depth snapshot and print top of book");
	static std::string symbol;
	static std::string file;
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
		"Stream a Binance market-data WebSocket to a JSONL file");
	static std::string symbol;
	static std::string outfile;
	static std::string speed  = "100ms";
	static std::string stream = "depth";
	static int seconds        = 30;
	static bool insecure_tls  = false;
	cap->add_option("symbol", symbol, "Binance symbol")->required();
	cap->add_option("outfile", outfile, "Destination JSONL file")->required();
	cap->add_option("--seconds", seconds, "Recording duration (seconds)")
		->capture_default_str();
	cap->add_option("--stream",
					stream,
					"Which stream to record: depth diffs, or the trade tape")
		->capture_default_str()
		->check(CLI::IsMember({"depth", "trade"}));
	cap->add_option("--speed", speed, "Update cadence (depth only)")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	cap->add_flag("--insecure-tls",
				  insecure_tls,
				  "Do not verify the stream's TLS certificate. For a host with "
				  "no CA bundle; a capture taken over an unverified stream is "
				  "replayed and backtested against later");
	cap->callback([&, cap] {
		// Only the parser can tell an option that was typed from one that came
		// from its default, which is why the warning lives here rather than in
		// cmd_capture. A cadence silently discarded is how an operator ends up
		// with a recording that is not the one they asked for.
		if (stream == "trade" && cap->count("--speed") > 0)
			spdlog::warn("--speed does not apply to the trade stream: the "
						 "venue pushes a message per fill, not on a timer");
		rc = cmd_capture(symbol, outfile, seconds, speed, stream, insecure_tls);
	});
}

void add_trades(CLI::App &app, int &rc) {
	auto *trades = app.add_subcommand(
		"trades",
		"Read a JSONL trade capture back as a tape: rate, burstiness, "
		"clustering");
	static std::string file;
	static int price_decimals            = 2;
	static int qty_decimals              = 2;
	static int bucket_ms                 = 1000;
	static unsigned long long max_trades = 0;
	trades->add_option("file", file, "JSONL capture of trade frames")
		->required()
		->check(CLI::ExistingFile);
	trades->add_option("--price-decimals", price_decimals, "Price scale")
		->capture_default_str();
	trades->add_option("--qty-decimals", qty_decimals, "Quantity scale")
		->capture_default_str();
	trades
		->add_option("--bucket-ms",
					 bucket_ms,
					 "Width of the buckets the rate distribution is measured "
					 "over. A tape is bursty enough that this changes the "
					 "answer: the same recording is steady at 1000ms and "
					 "violently uneven at 100ms")
		->capture_default_str();
	trades
		->add_option("--trades",
					 max_trades,
					 "Stop after this many prints (0 = the whole file)")
		->capture_default_str();
	trades->callback([&] {
		rc = cmd_trades(file,
						price_decimals,
						qty_decimals,
						bucket_ms,
						max_trades);
	});
}

void add_account(CLI::App &app, int &rc) {
	auto *account = app.add_subcommand(
		"account",
		"Authenticate against the venue and report what is working. Places "
		"nothing");
	static account_settings settings;

	// Environment-only, and attached before the flags so it is plain that the
	// credential is not one. @see credentials_option.hpp
	add_credentials(*account, settings.credential);

	account->add_option("--symbol", settings.symbol, "Binance symbol")
		->capture_default_str();
	// Flags rather than an option taking a value: `--live` has to be typed, and
	// `--env production` is one tab-completion away from being typed by
	// accident. The sandbox is the default and `--testnet` says so explicitly,
	// which is what a runbook or a CI job wants - a command whose meaning does
	// not depend on knowing what the default is.
	static bool live    = false;
	static bool testnet = false;
	static bool demo    = false;
	auto *live_flag =
		account->add_flag("--live",
						  live,
						  "Talk to PRODUCTION rather than the testnet sandbox. "
						  "Real account, real orders");
	// Mutually exclusive rather than last-one-wins: `--live --testnet` is a
	// command whose author did not know what it would do, and guessing at one
	// of the two is how it reaches the wrong account.
	account
		->add_flag("--testnet",
				   testnet,
				   "Talk to the testnet sandbox. The default; state it when a "
				   "script should not depend on that")
		->excludes(live_flag);
	// Demo mode is the one to measure a strategy in: fake balances against
	// depth that tracks the live exchange, where testnet's book is its own and
	// thin.
	// @see venue::environment
	auto *testnet_flag = account->get_option("--testnet");
	account
		->add_flag("--demo",
				   demo,
				   "Talk to Binance Demo Mode: fake money, but order books "
				   "that track the live exchange")
		->excludes(live_flag)
		->excludes(testnet_flag);
	// Destructive, so it is spelled out rather than implied by anything else -
	// and it needs no second confirmation flag: `--cancel-all` cannot be typed
	// by accident, and an incident is the wrong moment to make an operator type
	// a second thing before the orders come off.
	account->add_flag("--cancel-all",
					  settings.cancel_all,
					  "Withdraw every working order this engine placed. Orders "
					  "it did not place are reported and left alone");
	account->add_flag("--insecure-tls",
					  settings.insecure_tls,
					  "Do not verify the venue's TLS certificate. For a host "
					  "with no CA bundle - never with a credential on an "
					  "untrusted network");
	account->callback([&] {
		// `testnet` is not read: it is the default, and the exclusions above
		// are what make stating it meaningful rather than a third way to
		// choose. Only `--live` reaches an account with money in it.
		if (live) settings.env = venue::environment::production;
		else if (demo) settings.env = venue::environment::demo;
		else settings.env = venue::environment::testnet;
		rc = cmd_account(settings);
	});
}

void add_live(CLI::App &app, int &rc) {
	auto *live = app.add_subcommand(
		"live",
		"Track a Binance book live: diff stream plus on-demand snapshots");
	static std::string symbol;
	static std::string speed  = "100ms";
	static int seconds        = 30;
	static int limit          = 100;
	static int price_decimals = 2;
	static int qty_decimals   = 2;
	static int depth          = 10;
	static bool insecure_tls  = false;
	live->add_option("symbol", symbol, "Binance symbol")->required();
	live->add_option("--seconds",
					 seconds,
					 "How long to track; 0 runs until the stream ends")
		->capture_default_str();
	live->add_option("--speed", speed, "Update cadence")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	live->add_flag("--insecure-tls",
				   insecure_tls,
				   "Do not verify the feed's TLS certificate. For a host with "
				   "no CA bundle");
	live->add_option(
			"--limit",
			limit,
			"REST snapshot depth per side. Above the replica's retained "
			"depth (l2_book keeps 128 a side) the surplus is fetched, "
			"parsed and then dropped")
		->capture_default_str();
	live->add_option("--price-decimals", price_decimals, "Tick precision")
		->capture_default_str();
	live->add_option("--qty-decimals", qty_decimals, "Step precision")
		->capture_default_str();
	live->add_option("--depth",
					 depth,
					 "Ladder rows per side to print at the end (0 = all, which "
					 "is up to the 128 the replica retains)")
		->capture_default_str();
	live->callback([&] {
		rc = cmd_live(symbol,
					  seconds,
					  speed,
					  limit,
					  price_decimals,
					  qty_decimals,
					  depth,
					  insecure_tls);
	});
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
	// One static aggregate keeps every option's storage alive past this call,
	// the same way add_backtest does. @see the note above add_snapshot.
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
	bt->add_flag("--front-of-queue",
				 settings.front_of_queue,
				 "Ignore the venue's own liquidity resting ahead of ours at "
				 "our price, so every order fills as though it were first in "
				 "line. The most flattering assumption available - the "
				 "'queue' line in the report is what it is worth");
	bt->add_option("--latency-ns",
				   settings.latency_ns,
				   "Market-time nanoseconds a command spends in flight before "
				   "the engine has it. Set it to the whole round trip: the "
				   "market_data delay and the order delay enter the result "
				   "through their sum")
		->capture_default_str();
	bt->add_option("--jitter-ns",
				   settings.jitter_ns,
				   "Uniform extra flight time, drawn once per message. A "
				   "jittered run is one sample rather than a number - compare "
				   "like seeds with like")
		->capture_default_str();
	bt->add_option("--seed",
				   settings.seed,
				   "Seed for the jitter draw, and part of the run's identity. "
				   "Zero keeps the built-in one");
	bt->add_flag("--no-quote{false}",
				 settings.quote,
				 "Replay with no order flow - exercises the harness, not a "
				 "strategy; every fill counter must come back zero");

	bt->callback([&rc] { rc = cmd_backtest(settings); });
}

/**
 * @brief The three flags that turn a live run into a simulation.
 *
 * Split out of @c add_serve because they are the one group in it that changes
 * what a run *means* rather than how it is configured, and because `serve`
 * already declares more options than one function should.
 */
void add_serve_fill_model(CLI::App &serve, serve_settings &settings) {
	serve.add_flag(
		"--simulate-fills",
		settings.simulate_fills,
		"Infer the fills a resting order would have taken and inject them as "
		"aggressing flow, which is what makes --quote measurable. This is the "
		"one thing in a live run that is a judgement rather than the shipped "
		"engine - read strategy/backtest/fill_model.hpp before believing the "
		"numbers it produces");
	serve.add_flag("--fill-on-lock",
				   settings.fill_on_lock,
				   "With --simulate-fills: fill a resting order when the venue "
				   "quotes *at* its price, not only when it trades through. "
				   "Strictly more optimistic");
	serve.add_flag("--front-of-queue",
				   settings.front_of_queue,
				   "With --simulate-fills: ignore the venue's own liquidity "
				   "resting ahead of ours, so every order fills as though it "
				   "were first in line. The most flattering assumption "
				   "available; the 'queued' line is what it is worth");
	serve
		.add_option("--latency-ns",
					settings.latency_ns,
					"Wall-clock nanoseconds a command spends in flight before "
					"the engine has it, so a live run is comparable with a "
					"backtest's --latency-ns. Delivery is driven by a steady "
					"timer, so values below a millisecond arrive late and "
					"jittered - that floor is the platform's, not the model's")
		->capture_default_str();
	serve.add_option("--latency-jitter-ns",
					 settings.jitter_ns,
					 "Uniform extra flight time, drawn once per message");
	serve.add_option("--latency-seed",
					 settings.seed,
					 "Seed for the jitter draw. Zero keeps the built-in one");
}

/// @brief Which deployment `serve` talks to, and whether it sends anything.
///
/// The environment flags are spelled exactly as `account`'s and for the same
/// reason: `--env production` is one tab-completion away from being typed by
/// accident, and a flag has to be typed. What differs is the default. `account`
/// defaults to the sandbox because its whole job is the credentialed path;
/// `serve` defaults to production because its whole job is the *feed*, and the
/// sandbox's book is thin enough that a run against it measures nothing.
///
/// That default is only safe next to @c --send-orders being off, and the pair
/// is what @c cmd_serve refuses when an environment was not named.
///
/// @param live, testnet, demo Set by the flags; read back in the caller's
///        callback, which is where the three collapse into one
///        @c environment.
void add_serve_order_entry(CLI::App &serve, serve_settings &settings,
						   bool &live, bool &testnet, bool &demo) {
	auto *live_flag = serve.add_flag(
		"--live",
		live,
		"Talk to PRODUCTION. With --send-orders these are real orders on a "
		"real account");
	// Mutually exclusive rather than last-one-wins: `--live --testnet` is a
	// command whose author did not know what it would do.
	serve
		.add_flag("--testnet",
				  testnet,
				  "Talk to the testnet sandbox: its own book, its own thin "
				  "liquidity. Fine for proving an order is well formed, not "
				  "for pricing one")
		->excludes(live_flag);
	serve
		.add_flag("--demo",
				  demo,
				  "Talk to Binance Demo Mode: fake balances against order "
				  "books that track the live exchange. The one to measure a "
				  "strategy in")
		->excludes(live_flag)
		->excludes(serve.get_option("--testnet"));

	serve.add_flag(
		"--send-orders",
		settings.send_orders,
		"Send this session's orders to the venue instead of only matching them "
		"internally, and book the fills it reports back. Refused unless "
		"--testnet, --demo or --live was given, so the production default "
		"cannot quietly become production order entry");
	serve
		.add_option("--max-orders",
					settings.max_orders,
					"Orders this run may place in total; 0 is unlimited. The "
					"blunt bound on a strategy that quotes in a loop - it is "
					"the number an operator chose rather than a failure the "
					"risk gate happened to model")
		->capture_default_str();
	serve
		.add_option("--weight-reserve",
					settings.weight_reserve,
					"Rate-limit weight to leave unspent for market data. Order "
					"entry and depth snapshots share one per-IP allowance, and "
					"a run that spends it all cannot fetch its next resync")
		->capture_default_str();
}

void add_serve(CLI::App &app, int &rc,
			   const core::metrics::settings &metrics_settings) {
	auto *serve = app.add_subcommand(
		"serve",
		"Run the live path: a venue's feed through the whole engine, until "
		"stopped");
	// Static for the reason add_backtest's is: one aggregate keeps every
	// option's storage alive until CLI11 parses, instead of two dozen locals.
	static serve_settings settings;
	// The three environment flags' storage, on the same terms.
	static bool live    = false;
	static bool testnet = false;
	static bool demo    = false;

	// --- the credential, from the environment and nowhere else -------------
	// Attached before the flags below so it is obvious that it is not one:
	// these two options have no flag name at all. @see credentials_option.hpp
	add_credentials(*serve, settings.credential);

	// --- the listing and the feed -----------------------------------------
	serve->add_option("symbol", settings.symbol, "Binance symbol")
		->capture_default_str();
	serve
		->add_option("--seconds",
					 settings.seconds,
					 "How long to run; 0 runs until interrupted")
		->capture_default_str();
	serve->add_option("--speed", settings.speed, "Diff-stream cadence")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	serve->add_flag("--insecure-tls",
					settings.insecure_tls,
					"Do not verify the feed's TLS certificate. For a host with "
					"no CA bundle; an unverified feed can be dictated by "
					"whoever terminates the connection");
	serve->add_option("--limit", settings.limit, "REST snapshot depth per side")
		->capture_default_str();
	serve
		->add_option("--price-decimals", settings.price_decimals, "Price scale")
		->capture_default_str();
	serve->add_option("--qty-decimals", settings.qty_decimals, "Quantity scale")
		->capture_default_str();
	serve
		->add_option("--tick",
					 settings.tick,
					 "Price increment, e.g. 0.01. Must be exact at "
					 "--price-decimals")
		->capture_default_str();
	serve
		->add_option(
			"--lot",
			settings.lot,
			"Size increment, e.g. 0.01. Must be exact at --qty-decimals")
		->capture_default_str();
	serve->add_option("--reference",
					  settings.reference,
					  "Session anchor for the listing's collar. Inert here - "
					  "serve configures no collar - so the default is one tick "
					  "rather than a guess at the instrument's price");
	serve
		->add_option("--reconnect-ms",
					 settings.reconnect_ms,
					 "Pause before rebuilding a dropped stream")
		->capture_default_str();
	serve
		->add_option("--max-reconnects",
					 settings.max_reconnects,
					 "Reconnect attempts before giving up (0 = keep trying)")
		->capture_default_str();

	// --- the strategy ------------------------------------------------------
	serve->add_flag(
		"--take,!--quote",
		settings.take,
		"Cross the venue's touch with an IOC (--take) or rest inside it "
		"(--quote). On its own --quote fills nothing: the depth a bridge seeds "
		"is rested without matching, so there is nothing for a resting order "
		"to "
		"trade against. Pair it with --simulate-fills. @see quoter_options");
	serve->add_flag(
		"--venue-grid,!--no-venue-grid",
		settings.venue_grid,
		"Read the tick and step size from /api/v3/exchangeInfo instead of "
		"trusting --tick/--lot (default on). The flag defaults are wrong for "
		"almost every listing and wrong silently - surplus decimals are "
		"truncated, so a step coarser than the venue's rounds small levels to "
		"zero. Use --no-venue-grid to run without asking the venue");
	add_serve_fill_model(*serve, settings);
	serve
		->add_option("--improve",
					 settings.improve_ticks,
					 "Ticks inside the touch to quote, when --quote")
		->capture_default_str();
	serve->add_option("--lots", settings.lots, "Order size, in lots")
		->capture_default_str();
	serve
		->add_option("--requote-ms",
					 settings.requote_ms,
					 "Market time an order is left standing (0 = every frame)")
		->capture_default_str();

	// --- pre-trade risk ----------------------------------------------------
	serve
		->add_option("--max-position",
					 settings.max_position,
					 "Largest absolute net position, in lots (0 = open)")
		->capture_default_str();
	serve
		->add_option("--max-order-qty",
					 settings.max_order_qty,
					 "Largest quantity one order may carry (0 = open)")
		->capture_default_str();
	serve
		->add_option("--price-band",
					 settings.price_band_bps,
					 "Fat-finger band around the last print, in basis points "
					 "(0 disables)")
		->capture_default_str();
	serve
		->add_option("--max-loss",
					 settings.max_loss,
					 "Loss that trips the breaker, in tick-lots (0 disables)")
		->capture_default_str();
	serve
		->add_option("--breaches-to-trip",
					 settings.breaches_to_trip,
					 "Risk refusals in one window that trip the breaker "
					 "(0 = manual only)")
		->capture_default_str();

	// --- post-trade surveillance -------------------------------------------
	serve
		->add_option("--max-otr",
					 settings.max_messages_per_execution,
					 "Messages per execution that trip the breaker "
					 "(0 disables)")
		->capture_default_str();
	serve
		->add_option("--max-fills-per-window",
					 settings.max_executions_per_window,
					 "Executions in one burst window that trip (0 disables)")
		->capture_default_str();
	serve
		->add_option("--max-adverse-run",
					 settings.max_adverse_run,
					 "Same-direction prints that trip (0 disables)")
		->capture_default_str();
	serve
		->add_option("--burst-window-ms",
					 settings.burst_window_ms,
					 "Width of the burst window (0 = ~1 ms). Without this "
					 "--max-fills-per-window is unreachable at feed cadence: a "
					 "1 ms window never holds two executions. Rounded up to a "
					 "power of two")
		->capture_default_str();
	serve
		->add_option("--otr-window-ms",
					 settings.otr_window_ms,
					 "Width of the order-to-trade window (0 = ~1.07 s). "
					 "Rounded up to a power of two")
		->capture_default_str();
	serve
		->add_option("--min-otr-messages",
					 settings.min_otr_messages,
					 "Messages a window must hold before the ratio is judged "
					 "(0 = 100). A short window plus the default floor is a "
					 "rule that can never fire")
		->capture_default_str();
	serve
		->add_option("--outcome-timeout-ms",
					 settings.outcome_timeout_ms,
					 "Order-return-path silence, with orders working, that "
					 "trips (0 disables)")
		->capture_default_str();

	// --- the system lane ---------------------------------------------------
	serve
		->add_option("--feed-timeout-ms",
					 settings.feed_timeout_ms,
					 "market_data silence that trips the breaker (0 disables). "
					 "Two missed frames is a diagnosis; a quiet market is not")
		->capture_default_str();

	add_serve_order_entry(*serve, settings, live, testnet, demo);

	serve->callback([&rc, &metrics_settings] {
		// `testnet` is not read for the same reason `account`'s is not: the
		// exclusions are what make stating it meaningful, and only `--live`
		// reaches an account with money in it. The default here is *production*
		// rather than the sandbox, and that is about the feed - reading the
		// real book is what this command has always done. It is safe as a
		// default only because order entry is refused unless one of the three
		// was typed, which is checked below rather than here so the message can
		// name all three.
		settings.env_chosen = live || demo || testnet;
		if (live) settings.env = venue::environment::production;
		else if (demo) settings.env = venue::environment::demo;
		else if (testnet) settings.env = venue::environment::testnet;
		rc = cmd_serve(settings, metrics_settings);
	});
}

} // namespace exchange::app
