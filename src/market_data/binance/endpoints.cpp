#include "endpoints.hpp"

#include "core/util/lowered.hpp"

#include <fmt/format.h>

namespace exchange::market_data::binance {

using core::util::lowered;

stream_endpoint diff_depth_stream(std::string_view symbol, depth_speed speed) {
	// A raw single stream is /ws/<streamName>; the faster variant appends the
	// cadence to it. Taking that suffix from the enum's own label (via its
	// format_as) keeps the wire form and the label from drifting apart.
	const std::string name = lowered(symbol);
	return {.host   = "stream.binance.com",
			.port   = "9443",
			.target = speed == depth_speed::every_100ms
						  ? fmt::format("/ws/{}@depth@{}", name, speed)
						  : fmt::format("/ws/{}@depth", name)};
}

http_endpoint depth_snapshot_endpoint(std::string_view symbol, int limit) {
	return {.host = "api.binance.com",
			.target =
				fmt::format("/api/v3/depth?symbol={}&limit={}", symbol, limit)};
}

http_endpoint exchange_info(std::string_view symbol) {
	return {.host   = "api.binance.com",
			.target = fmt::format("/api/v3/exchangeInfo?symbol={}", symbol)};
}

} // namespace exchange::market_data::binance
