#include "capture.hpp"

#include "core/logging.hpp"
#include "market_data.hpp"
#include "transport.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::app {

// Capture the venue's published diff-depth feed: the `<symbol>@depth` stream
// whose frames carry absolute aggregate sizes per price. market-data decides
// which endpoint that is; transport just records the frames.
int cmd_capture(const std::string &symbol, const std::string &outfile,
				int seconds, std::string_view speed) {
	namespace binance = market_data::binance;

	if (seconds <= 0) {
		spdlog::error("seconds must be positive (got {})", seconds);
		return EXIT_FAILURE;
	}

	auto [host, port, target] = binance::diff_depth_stream(
		symbol,
		speed == "1000ms" ? binance::depth_speed::every_1000ms
						  : binance::depth_speed::every_100ms);

	spdlog::info("capturing {} @{} for {}s from {} -> {}",
				 symbol,
				 speed,
				 seconds,
				 host,
				 outfile);

	const auto result =
		exchange::transport::ws::capture(std::move(host),
										 std::move(port),
										 std::move(target),
										 outfile,
										 std::chrono::seconds(seconds));
	if (!result) {
		spdlog::error("capture failed: {}", result.error());
		return EXIT_FAILURE;
	}
	spdlog::info("capture complete: {}", outfile);
	return EXIT_SUCCESS;
}

} // namespace exchange::app
