// Option wiring for `exchange_tool demo`.
//
// The registrar and the option state it binds; the driver it
// finally calls lives in app/commands/demo.cpp. @see app/cli.hpp
// for the lifetime rule every registrar here obeys.

#include "app/cli.hpp"

#include "app/commands/demo.hpp"
#include "core/metrics/settings.hpp"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <memory>

namespace exchange::app {

namespace {
struct demo_options {
	std::uint64_t num_orders = 2'000'000;
};
} // namespace

void add_demo(CLI::App &app, int &rc,
			  const core::metrics::settings &metrics_settings) {
	auto *demo = app.add_subcommand(
		"demo",
		"Run the MatchingEngine end-to-end over the SPSC queue");

	auto state = std::make_shared<demo_options>();
	demo->add_option("num_orders",
					 state->num_orders,
					 "Synthetic orders to submit")
		->capture_default_str();
	demo->callback([&rc, &metrics_settings, state] {
		rc = cmd_demo(state->num_orders, metrics_settings);
	});
}

} // namespace exchange::app
