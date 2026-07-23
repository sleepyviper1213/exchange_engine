//   exchange_tool snapshot SYMBOL [--limit N] [--price-decimals N] [--qty-decimals N]
//   exchange_tool snapshot --file <depth.json> [--price-decimals N] [--qty-decimals N]
//   exchange_tool capture  SYMBOL OUTFILE [--seconds N] [--speed 100ms|1000ms]
//   exchange_tool replay   FILE.jsonl [--snapshot seed.json] [--price-decimals N] [--qty-decimals N]
//   exchange_tool demo     [num_orders]

#include "version.hpp"
#include "cli.hpp"
#include "configuration.hpp"
#include "logger.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
    const core::Configuration config = core::Configuration::from_env();
    core::init_logging(config);

    CLI::App app{"exchange_tool -- order-book market-data & engine CLI"};
    app.set_version_flag("--version", std::string{core::version});
    app.require_subcommand(1);
    int rc = EXIT_SUCCESS;

    cli::add_snapshot(app, rc); // fetch/load a depth snapshot → book → top of book
    cli::add_capture(app, rc);  // stream a diff-depth WebSocket to a JSONL file
    cli::add_replay(app, rc);   // replay a JSONL capture through an OrderBook
    cli::add_demo(app, rc);     // run the MatchingEngine end-to-end

    CLI11_PARSE(app, argc, argv);
    return rc;
}
