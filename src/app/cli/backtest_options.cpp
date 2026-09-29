// Option wiring for `exchange_tool backtest`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/backtest.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"
#include "app/commands/backtest.hpp"

#include <CLI/CLI.hpp>

#include <memory>

namespace exchange::app {

namespace {
struct backtest_options {
	backtest_settings settings;
};
} // namespace

void add_backtest(CLI::App &app, int &rc) {
	auto *bt = app.add_subcommand(
		"backtest",
		"Run a strategy against a recorded capture through the whole engine");

	auto state = std::make_shared<backtest_options>();

	bt->add_option("file",
				   state->settings.file,
				   "JSONL capture of depthUpdate frames")
		->required()
		->check(CLI::ExistingFile);
	bt->add_option("--snapshot",
				   state->settings.snapshot,
				   "REST depth JSON seeding the replica. Required: without a "
				   "seed the book only holds prices the capture happened to "
				   "touch, and nothing meaningful can fill against it")
		->required()
		->check(CLI::ExistingFile);
	bt->add_option("--symbol",
				   state->settings.symbol,
				   "Display name for the listing")
		->capture_default_str();
	bt->add_option("--price-decimals",
				   state->settings.price_decimals,
				   "Price scale")
		->capture_default_str();
	bt->add_option("--qty-decimals",
				   state->settings.qty_decimals,
				   "Quantity scale")
		->capture_default_str();
	bt->add_option(
		  "--tick",
		  state->settings.tick,
		  "Price increment, e.g. 0.01. Must be exact at --price-decimals")
		->capture_default_str();
	bt->add_option("--lot",
				   state->settings.lot,
				   "Size increment, e.g. 0.01. Must be exact at --qty-decimals")
		->capture_default_str();
	bt->add_option("--events",
				   state->settings.events,
				   "Stop after this many frames (0 = the whole file)")
		->capture_default_str();
	bt->add_option("--improve",
				   state->settings.improve_ticks,
				   "Ticks inside the venue's touch to quote. Below 1 the quote "
				   "can never be traded through and will never fill passively")
		->capture_default_str();
	bt->add_option("--lots", state->settings.lots, "Quote size, in lots")
		->capture_default_str();
	bt->add_option(
		  "--requote-ms",
		  state->settings.requote_ms,
		  "Market time a quote is left standing before it is replaced. "
		  "Zero requotes on every frame, which is also why zero fills "
		  "nothing: the quoter steps out of the way before the market "
		  "ever reaches it, and being slower is what creates exposure")
		->capture_default_str();
	bt->add_option("--max-position",
				   state->settings.max_position,
				   "Risk limit on absolute net position, in lots (0 = open)")
		->capture_default_str();
	bt->add_flag("--fill-on-lock",
				 state->settings.fill_on_lock,
				 "Fill a resting order when the venue quotes *at* its price, "
				 "not only when it trades through. Strictly more optimistic");
	bt->add_flag("--front-of-queue",
				 state->settings.front_of_queue,
				 "Ignore the venue's own liquidity resting ahead of ours at "
				 "our price, so every order fills as though it were first in "
				 "line. The most flattering assumption available - the "
				 "'queue' line in the report is what it is worth");
	bt->add_flag("--markout",
				 state->settings.markout,
				 "Score every fill against the midpoint at a set of horizons "
				 "after it, and print the curve. A passive markout that turns "
				 "negative as the horizon lengthens is adverse selection - the "
				 "fills are arriving because the market is about to move, not "
				 "because the quote was early");
	bt->add_option("--tape",
				   state->settings.tape,
				   "A JSONL trade capture covering the same window as the "
				   "depth file. Every modelled passive fill is then checked "
				   "against the prints that would justify it, and the volume "
				   "with nothing behind it is reported. Record the two "
				   "concurrently: `capture --stream trade` alongside "
				   "`capture --stream depth`")
		->check(CLI::ExistingFile);
	bt->add_option("--tape-tolerance-ms",
				   state->settings.tape_tolerance_ms,
				   "Half-window the audit matches within. Defaults to one "
				   "100ms depth frame, which is the interval a fill inferred "
				   "from a frame could actually have happened in; a capture "
				   "taken at a different cadence wants a different value")
		->capture_default_str();
	bt->add_option("--latency-ns",
				   state->settings.latency_ns,
				   "Market-time nanoseconds a command spends in flight before "
				   "the engine has it. Set it to the whole round trip: the "
				   "market_data delay and the order delay enter the result "
				   "through their sum")
		->capture_default_str();
	bt->add_option("--jitter-ns",
				   state->settings.jitter_ns,
				   "Uniform extra flight time, drawn once per message. A "
				   "jittered run is one sample rather than a number - compare "
				   "like seeds with like")
		->capture_default_str();
	bt->add_option("--seed",
				   state->settings.seed,
				   "Seed for the jitter draw, and part of the run's identity. "
				   "Zero keeps the built-in one");
	bt->add_flag("--no-quote{false}",
				 state->settings.quote,
				 "Replay with no order flow - exercises the harness, not a "
				 "strategy; every fill counter must come back zero");

	bt->callback([&rc, state] { rc = cmd_backtest(state->settings); });
}

} // namespace exchange::app
