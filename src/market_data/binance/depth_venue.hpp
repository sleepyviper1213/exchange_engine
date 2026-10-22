#pragma once
// Binance spot's diff-depth feed as a market_data::depth_venue: the stream and
// snapshot endpoints, the snapshot parse and normalise, the error envelope,
// and the frame decoder. The wire format only.

#include "market_data/binance/depth_feed.hpp"
#include "market_data/binance/depth_speed.hpp"
#include "market_data/binance/endpoints.hpp"
#include "market_data/binance/normalise.hpp"
#include "market_data/binance/parse_depth.hpp"
#include "market_data/depth_venue.hpp"
#include "venue/binance/api_error.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::market_data::binance {

struct depth_venue {
	using cadence = depth_speed;
	using decoder = depth_frame_decoder;

	static constexpr cadence DEFAULT_CADENCE = depth_speed::every_100ms;

	[[nodiscard]] static venue::stream_endpoint
	stream(std::string_view symbol, cadence speed, venue::environment env) {
		return diff_depth_stream(symbol, speed, env);
	}

	[[nodiscard]] static venue::http_endpoint
	snapshot_endpoint(std::string_view symbol, int limit,
					  venue::environment env) {
		return depth_snapshot_endpoint(symbol, limit, env);
	}

	/// Parsed and normalised in one step: the pipeline wants the neutral
	/// snapshot, and the venue's own shape is nobody else's business.
	[[nodiscard]] static std::expected<book_snapshot, std::string>
	parse_snapshot(std::string_view body, int price_decimals,
				   int qty_decimals) {
		auto parsed = parse_binance_depth(body, price_decimals, qty_decimals);
		if (!parsed) return std::unexpected(message(parsed.error()));
		return normalise(std::move(*parsed));
	}

	/// The venue's own words where it gave any: "Invalid symbol." beats
	/// "HTTP 400: {json}" for whoever has to fix it.
	[[nodiscard]] static std::string describe_error(std::string_view body,
													std::string_view fallback) {
		return venue::binance::describe_api_error(body, fallback);
	}
};

static_assert(market_data::depth_venue<depth_venue>);

} // namespace exchange::market_data::binance
