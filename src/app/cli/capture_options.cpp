// Option wiring for `exchange_tool capture`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/capture.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/capture.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace exchange::app {

namespace {
struct capture_options {
	std::string symbol;
	std::string outfile;
	std::string speed  = "100ms";
	std::string stream = "depth";
	int seconds        = 30;
	bool insecure_tls  = false;
};
} // namespace

void add_capture(CLI::App &app, int &rc) {
	auto *cap = app.add_subcommand(
		"capture",
		"Stream a Binance market-data WebSocket to a JSONL file");

	auto state = std::make_shared<capture_options>();
	cap->add_option("symbol", state->symbol, "Binance symbol")->required();
	cap->add_option("outfile", state->outfile, "Destination JSONL file")
		->required();
	cap->add_option("--seconds", state->seconds, "Recording duration (seconds)")
		->capture_default_str();
	cap->add_option("--stream",
					state->stream,
					"Which stream to record: depth diffs, or the trade tape")
		->capture_default_str()
		->check(CLI::IsMember({"depth", "trade"}));
	cap->add_option("--speed", state->speed, "Update cadence (depth only)")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	cap->add_flag("--insecure-tls",
				  state->insecure_tls,
				  "Do not verify the stream's TLS certificate. For a host with "
				  "no CA bundle; a capture taken over an unverified stream is "
				  "replayed and backtested against later");
	cap->callback([&rc, cap, state] {
		// Only the parser can tell an option that was typed from one that came
		// from its default, which is why the warning lives here rather than in
		// cmd_capture. A cadence silently discarded is how an operator ends up
		// with a recording that is not the one they asked for.
		if (state->stream == "trade" && cap->count("--speed") > 0)
			spdlog::warn("--speed does not apply to the trade stream: the "
						 "venue pushes a message per fill, not on a timer");
		rc = cmd_capture(state->symbol,
						 state->outfile,
						 state->seconds,
						 state->speed,
						 state->stream,
						 state->insecure_tls);
	});
}

} // namespace exchange::app
