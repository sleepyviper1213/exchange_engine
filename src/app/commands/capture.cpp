#include "capture.hpp"

#include "app/cadence_option.hpp"
#include "market_data.hpp"
#include "transport.hpp"
#include "venue/endpoint.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>


namespace exchange::app {
namespace {

namespace binance = market_data::binance;

/// The streams `capture` knows how to record.
///
/// Deliberately a CLI-level list rather than an enum in market_data: the module
/// already owns the endpoint builders, and what stays here is only *which of
/// them this subcommand offers*. A venue gaining a third stream adds a builder
/// there and a line here, and the two stay independent.
constexpr std::string_view DEPTH_STREAM = "depth";
constexpr std::string_view TRADE_STREAM = "trade";

/// Resolve @p stream to the endpoint that publishes it, or nullopt if the name
/// is not one this command records.
std::optional<venue::stream_endpoint>
endpoint_for(std::string_view stream, const std::string &symbol,
			 binance::depth_speed cadence) {
	if (stream == DEPTH_STREAM)
		return binance::diff_depth_stream(symbol, cadence);
	if (stream == TRADE_STREAM) return binance::trade_stream(symbol);
	return std::nullopt;
}

} // namespace

// Capture one of the venue's published market-data streams. market_data decides
// which endpoint that is; transport just records the frames, and neither knows
// what the other's bytes mean.
int cmd_capture(const std::string &symbol, const std::string &outfile,
				int seconds, std::string_view speed, std::string_view stream,
				bool insecure_tls) {
	if (seconds <= 0) {
		spdlog::error("seconds must be positive (got {})", seconds);
		return EXIT_FAILURE;
	}

	const auto cadence = cadence_from(speed, "speed");
	if (!cadence) return EXIT_FAILURE;

	auto endpoint = endpoint_for(stream, symbol, *cadence);
	if (!endpoint) {
		spdlog::error("unknown stream \"{}\": want {} or {}",
					  stream,
					  DEPTH_STREAM,
					  TRADE_STREAM);
		return EXIT_FAILURE;
	}

	// Whether the operator *typed* --speed on a tape capture is a question only
	// the parser can answer - the option has a default, so by the time it
	// reaches here every call carries one. add_capture warns about that; this
	// function just records what it was asked for.

	spdlog::info("capturing {} {} for {}s from {} -> {}",
				 symbol,
				 endpoint->target,
				 seconds,
				 endpoint->host,
				 outfile);

	const auto result = exchange::transport::ws::capture(
		std::move(endpoint->host),
		std::move(endpoint->port),
		std::move(endpoint->target),
		outfile,
		std::chrono::seconds(seconds),
		insecure_tls ? transport::tls_verify::none
					 : transport::tls_verify::peer);
	if (!result) {
		spdlog::error("capture failed: {}", result.error());
		return EXIT_FAILURE;
	}
	spdlog::info("capture complete: {}", outfile);
	return EXIT_SUCCESS;
}

} // namespace exchange::app
