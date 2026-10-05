// Option wiring for `exchange_tool serve`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/serve.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/serve.hpp"
#include "app/credentials_option.hpp"
#include "core/metrics/settings.hpp"

#include <CLI/CLI.hpp>

#include <memory>

namespace exchange::app {

namespace {
struct serve_options {
	serve_settings settings;
	bool live    = false;
	bool testnet = false;
	bool demo    = false;
};
} // namespace

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
				  "books that track the live order_book. The one to measure a "
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

	auto state = std::make_shared<serve_options>();

	// --- the credential, from the environment and nowhere else -------------
	// Attached before the flags below so it is obvious that it is not one:
	// these two options have no flag name at all. @see credentials_option.hpp
	add_credentials(*serve, state->settings.credential);

	// --- the listing and the feed -----------------------------------------
	serve->add_option("symbol", state->settings.symbol, "Binance symbol")
		->capture_default_str();
	serve
		->add_option("--seconds",
					 state->settings.seconds,
					 "How long to run; 0 runs until interrupted")
		->capture_default_str();
	serve->add_option("--speed", state->settings.speed, "Diff-stream cadence")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	serve->add_flag("--insecure-tls",
					state->settings.insecure_tls,
					"Do not verify the feed's TLS certificate. For a host with "
					"no CA bundle; an unverified feed can be dictated by "
					"whoever terminates the connection");
	serve
		->add_option("--limit",
					 state->settings.limit,
					 "REST snapshot depth per side")
		->capture_default_str();
	serve
		->add_option("--price-decimals",
					 state->settings.price_decimals,
					 "Price scale")
		->capture_default_str();
	serve
		->add_option("--qty-decimals",
					 state->settings.qty_decimals,
					 "Quantity scale")
		->capture_default_str();
	serve
		->add_option("--tick",
					 state->settings.tick,
					 "Price increment, e.g. 0.01. Must be exact at "
					 "--price-decimals")
		->capture_default_str();
	serve
		->add_option(
			"--lot",
			state->settings.lot,
			"Size increment, e.g. 0.01. Must be exact at --qty-decimals")
		->capture_default_str();
	serve->add_option("--reference",
					  state->settings.reference,
					  "Session anchor for the listing's collar. Inert here - "
					  "serve configures no collar - so the default is one tick "
					  "rather than a guess at the instrument's price");
	serve
		->add_option("--reconnect-ms",
					 state->settings.reconnect_ms,
					 "Pause before rebuilding a dropped stream")
		->capture_default_str();
	serve
		->add_option("--max-reconnects",
					 state->settings.max_reconnects,
					 "Reconnect attempts before giving up (0 = keep trying)")
		->capture_default_str();

	// --- the strategy ------------------------------------------------------
	serve->add_flag(
		"--take,!--quote",
		state->settings.take,
		"Cross the venue's touch with an IOC (--take) or rest inside it "
		"(--quote). On its own --quote fills nothing: the depth a bridge seeds "
		"is rested without matching, so there is nothing for a resting order "
		"to "
		"trade against. Pair it with --simulate-fills. @see quoter_options");
	serve->add_flag(
		"--venue-grid,!--no-venue-grid",
		state->settings.venue_grid,
		"Read the tick and step size from /api/v3/exchangeInfo instead of "
		"trusting --tick/--lot (default on). The flag defaults are wrong for "
		"almost every listing and wrong silently - surplus decimals are "
		"truncated, so a step coarser than the venue's rounds small levels to "
		"zero. Use --no-venue-grid to run without asking the venue");
	add_serve_fill_model(*serve, state->settings);
	serve
		->add_option("--improve",
					 state->settings.improve_ticks,
					 "Ticks inside the touch to quote, when --quote")
		->capture_default_str();
	serve->add_option("--lots", state->settings.lots, "Order size, in lots")
		->capture_default_str();
	serve
		->add_option("--requote-ms",
					 state->settings.requote_ms,
					 "Market time an order is left standing (0 = every frame)")
		->capture_default_str();

	// --- pre-trade risk ----------------------------------------------------
	serve
		->add_option("--max-position",
					 state->settings.max_position,
					 "Largest absolute net position, in lots (0 = open)")
		->capture_default_str();
	serve
		->add_option("--max-order-qty",
					 state->settings.max_order_qty,
					 "Largest quantity one order may carry (0 = open)")
		->capture_default_str();
	serve
		->add_option("--price-band",
					 state->settings.price_band_bps,
					 "Fat-finger band around the last print, in basis points "
					 "(0 disables)")
		->capture_default_str();
	serve
		->add_option("--max-loss",
					 state->settings.max_loss,
					 "Loss that trips the breaker, in USDT, e.g. 25.5 "
					 "(0 disables)")
		->capture_default_str();
	serve
		->add_option("--breaches-to-trip",
					 state->settings.breaches_to_trip,
					 "Risk refusals in one window that trip the breaker "
					 "(0 = manual only)")
		->capture_default_str();

	// --- post-trade surveillance -------------------------------------------
	serve
		->add_option("--max-otr",
					 state->settings.max_messages_per_execution,
					 "Messages per execution that trip the breaker "
					 "(0 disables)")
		->capture_default_str();
	serve
		->add_option("--max-fills-per-window",
					 state->settings.max_executions_per_window,
					 "Executions in one burst window that trip (0 disables)")
		->capture_default_str();
	serve
		->add_option("--max-adverse-run",
					 state->settings.max_adverse_run,
					 "Same-direction prints that trip (0 disables)")
		->capture_default_str();
	serve
		->add_option("--burst-window-ms",
					 state->settings.burst_window_ms,
					 "Width of the burst window (0 = ~1 ms). Without this "
					 "--max-fills-per-window is unreachable at feed cadence: a "
					 "1 ms window never holds two executions. Rounded up to a "
					 "power of two")
		->capture_default_str();
	serve
		->add_option("--otr-window-ms",
					 state->settings.otr_window_ms,
					 "Width of the order-to-trade window (0 = ~1.07 s). "
					 "Rounded up to a power of two")
		->capture_default_str();
	serve
		->add_option("--min-otr-messages",
					 state->settings.min_otr_messages,
					 "Messages a window must hold before the ratio is judged "
					 "(0 = 100). A short window plus the default floor is a "
					 "rule that can never fire")
		->capture_default_str();
	serve
		->add_option("--outcome-timeout-ms",
					 state->settings.outcome_timeout_ms,
					 "Order-return-path silence, with orders working, that "
					 "trips (0 disables)")
		->capture_default_str();

	// --- the system lane ---------------------------------------------------
	serve
		->add_option("--feed-timeout-ms",
					 state->settings.feed_timeout_ms,
					 "market_data silence that trips the breaker (0 disables). "
					 "Two missed frames is a diagnosis; a quiet market is not")
		->capture_default_str();

	add_serve_order_entry(*serve,
						  state->settings,
						  state->live,
						  state->testnet,
						  state->demo);

	serve->callback([&rc, &metrics_settings, state] {
		// `testnet` is not read for the same reason `account`'s is not: the
		// exclusions are what make stating it meaningful, and only `--live`
		// reaches an account with money in it. The default here is *production*
		// rather than the sandbox, and that is about the feed - reading the
		// real book is what this command has always done. It is safe as a
		// default only because order entry is refused unless one of the three
		// was typed, which is checked below rather than here so the message can
		// name all three.
		state->settings.env_chosen =
			state->live || state->demo || state->testnet;
		if (state->live) state->settings.env = venue::environment::production;
		else if (state->demo) state->settings.env = venue::environment::demo;
		else if (state->testnet)
			state->settings.env = venue::environment::testnet;
		rc = cmd_serve(state->settings, metrics_settings);
	});
}

} // namespace exchange::app
