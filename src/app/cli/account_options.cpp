// Option wiring for `exchange_tool account`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/account.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/account.hpp"
#include "app/credentials_option.hpp"

#include <CLI/CLI.hpp>

#include <memory>

namespace exchange::app {

namespace {
struct account_options {
	account_settings settings;
	bool live    = false;
	bool testnet = false;
	bool demo    = false;
};
} // namespace

void add_account(CLI::App &app, int &rc) {
	auto *account = app.add_subcommand(
		"account",
		"Authenticate against the venue and report what is working. Places "
		"nothing");

	auto state = std::make_shared<account_options>();

	// Environment-only, and attached before the flags so it is plain that the
	// credential is not one. @see credentials_option.hpp
	add_credentials(*account, state->settings.credential);

	account->add_option("--symbol", state->settings.symbol, "Binance symbol")
		->capture_default_str();
	// Flags rather than an option taking a value: `--live` has to be typed, and
	// `--env production` is one tab-completion away from being typed by
	// accident. The sandbox is the default and `--testnet` says so explicitly,
	// which is what a runbook or a CI job wants - a command whose meaning does
	// not depend on knowing what the default is.
	auto *live_flag =
		account->add_flag("--live",
						  state->live,
						  "Talk to PRODUCTION rather than the testnet sandbox. "
						  "Real account, real orders");
	// Mutually exclusive rather than last-one-wins: `--live --testnet` is a
	// command whose author did not know what it would do, and guessing at one
	// of the two is how it reaches the wrong account.
	account
		->add_flag("--testnet",
				   state->testnet,
				   "Talk to the testnet sandbox. The default; state it when a "
				   "script should not depend on that")
		->excludes(live_flag);
	// Demo mode is the one to measure a strategy in: fake balances against
	// depth that tracks the live order_book, where testnet's book is its own and
	// thin.
	// @see venue::environment
	auto *testnet_flag = account->get_option("--testnet");
	account
		->add_flag("--demo",
				   state->demo,
				   "Talk to Binance Demo Mode: fake money, but order books "
				   "that track the live order_book")
		->excludes(live_flag)
		->excludes(testnet_flag);
	// Destructive, so it is spelled out rather than implied by anything else -
	// and it needs no second confirmation flag: `--cancel-all` cannot be typed
	// by accident, and an incident is the wrong moment to make an operator type
	// a second thing before the orders come off.
	account->add_flag("--cancel-all",
					  state->settings.cancel_all,
					  "Withdraw every working order this engine placed. Orders "
					  "it did not place are reported and left alone");
	account->add_flag("--insecure-tls",
					  state->settings.insecure_tls,
					  "Do not verify the venue's TLS certificate. For a host "
					  "with no CA bundle - never with a credential on an "
					  "untrusted network");
	account->callback([&rc, state] {
		// `testnet` is not read: it is the default, and the exclusions above
		// are what make stating it meaningful rather than a third way to
		// choose. Only `--live` reaches an account with money in it.
		if (state->live) state->settings.env = venue::environment::production;
		else if (state->demo) state->settings.env = venue::environment::demo;
		else state->settings.env = venue::environment::testnet;
		rc = cmd_account(state->settings);
	});
}

} // namespace exchange::app
