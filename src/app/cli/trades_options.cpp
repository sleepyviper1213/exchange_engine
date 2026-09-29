// Option wiring for `exchange_tool trades`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/trades.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/trades.hpp"

#include <CLI/CLI.hpp>

#include <memory>
#include <string>

namespace exchange::app {

namespace {
struct trades_options {
	std::string file;
	int price_decimals            = 2;
	int qty_decimals              = 2;
	int bucket_ms                 = 1000;
	unsigned long long max_trades = 0;
};
} // namespace

void add_trades(CLI::App &app, int &rc) {
	auto *trades = app.add_subcommand(
		"trades",
		"Read a JSONL trade capture back as a tape: rate, burstiness, "
		"clustering");

	auto state = std::make_shared<trades_options>();
	trades->add_option("file", state->file, "JSONL capture of trade frames")
		->required()
		->check(CLI::ExistingFile);
	trades->add_option("--price-decimals", state->price_decimals, "Price scale")
		->capture_default_str();
	trades->add_option("--qty-decimals", state->qty_decimals, "Quantity scale")
		->capture_default_str();
	trades
		->add_option("--bucket-ms",
					 state->bucket_ms,
					 "Width of the buckets the rate distribution is measured "
					 "over. A tape is bursty enough that this changes the "
					 "answer: the same recording is steady at 1000ms and "
					 "violently uneven at 100ms")
		->capture_default_str();
	trades
		->add_option("--trades",
					 state->max_trades,
					 "Stop after this many prints (0 = the whole file)")
		->capture_default_str();
	trades->callback([&rc, state] {
		rc = cmd_trades(state->file,
						state->price_decimals,
						state->qty_decimals,
						state->bucket_ms,
						state->max_trades);
	});
}

} // namespace exchange::app
