// Option wiring for `exchange_tool watchdog`.
//
// The registrar and the option state it binds; the driver it finally calls
// lives in app/commands/watchdog.cpp. @see app/cli.hpp for the lifetime rule
// every registrar here obeys.

#include "app/cli.hpp"
#include "app/commands/watchdog.hpp"
#include "app/credentials_option.hpp"

#include <CLI/CLI.hpp>

#include <memory>

namespace exchange::app {

namespace {
struct watchdog_options {
	watchdog_settings settings;
	bool live    = false;
	bool testnet = false;
	bool demo    = false;
};
} // namespace

void add_watchdog(CLI::App &app, int &rc) {
	auto *watchdog = app.add_subcommand(
		"watchdog",
		"The kill switch: watch a serve's --heartbeat-file and withdraw every "
		"order of ours if it goes silent. Run it beside any serve that sends "
		"orders - the venue has no cancel-on-disconnect for spot");

	auto state = std::make_shared<watchdog_options>();
	add_credentials(*watchdog, state->settings.credential);

	watchdog
		->add_option(
			"--heartbeat-file",
			state->settings.heartbeat_file,
			"The file the watched serve writes; give both the same path")
		->required();
	watchdog->add_option("--symbol", state->settings.symbol, "Binance symbol")
		->capture_default_str();
	watchdog
		->add_option("--stale-ms",
					 state->settings.stale_ms,
					 "Silence after which serve is taken to be dead")
		->check(CLI::PositiveNumber)
		->capture_default_str();
	watchdog
		->add_option("--poll-ms",
					 state->settings.poll_ms,
					 "How often to read the heartbeat")
		->check(CLI::PositiveNumber)
		->capture_default_str();

	// The same three mutually exclusive flags `account` takes, for the same
	// reasons: production has to be typed, and two of them at once is a
	// command whose author did not know which account it would touch.
	auto *live_flag = watchdog->add_flag("--live",
										 state->live,
										 "Watch a serve trading PRODUCTION");
	auto *testnet_flag =
		watchdog
			->add_flag("--testnet",
					   state->testnet,
					   "Watch a serve on the testnet sandbox. The default")
			->excludes(live_flag);
	watchdog
		->add_flag("--demo", state->demo, "Watch a serve in Binance Demo Mode")
		->excludes(live_flag)
		->excludes(testnet_flag);

	watchdog->callback([&rc, state] {
		if (state->live) state->settings.env = venue::environment::production;
		else if (state->demo) state->settings.env = venue::environment::demo;
		else state->settings.env = venue::environment::testnet;
		rc = cmd_watchdog(state->settings);
	});
}

} // namespace exchange::app
