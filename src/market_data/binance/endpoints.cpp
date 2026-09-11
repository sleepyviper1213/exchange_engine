#include "endpoints.hpp"

#include "core/util/lowered.hpp"
#include "venue/binance/host.hpp"

#include <fmt/format.h>

namespace exchange::market_data::binance {

using core::util::lowered;

venue::stream_endpoint diff_depth_stream(std::string_view symbol,
										 depth_speed speed,
										 venue::environment env) {
	// A raw single stream is /ws/<streamName>; the faster variant appends the
	// cadence to it. Taking that suffix from the enum's own label (via its
	// format_as) keeps the wire form and the label from drifting apart.
	const std::string name         = lowered(symbol);
	const venue::binance::hosts at = venue::binance::host_for(env);
	return {.host   = std::string(at.stream),
			.port   = std::string(at.stream_port),
			.target = speed == depth_speed::every_100ms
						  ? fmt::format("/ws/{}@depth@{}", name, speed)
						  : fmt::format("/ws/{}@depth", name)};
}

venue::stream_endpoint trade_stream(std::string_view symbol,
									venue::environment env) {
	// A raw single stream is /ws/<streamName>, and the tape has no cadence
	// suffix to choose: the venue pushes a message per fill rather than on a
	// timer, which is exactly why its burstiness survives into the recording.
	const venue::binance::hosts at = venue::binance::host_for(env);
	return {.host   = std::string(at.stream),
			.port   = std::string(at.stream_port),
			.target = fmt::format("/ws/{}@trade", lowered(symbol))};
}

venue::http_endpoint depth_snapshot_endpoint(std::string_view symbol, int limit,
											 venue::environment env) {
	return {.host = std::string(venue::binance::host_for(env).rest),
			.target =
				fmt::format("/api/v3/depth?symbol={}&limit={}", symbol, limit)};
}

} // namespace exchange::market_data::binance
