#include "capture.hpp"

#include "core/logging.hpp"
#include "market_data.hpp"
#include "transport.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::app {

// Capture the venue's published diff-depth feed: the `<symbol>@depth` stream
// whose frames carry absolute aggregate sizes per price. market_data decides
// which endpoint that is; transport just records the frames.
int cmd_capture(const std::string &symbol, const std::string &outfile,
				int seconds, std::string_view speed) {
	namespace binance = market_data::binance;

	if (seconds <= 0) {
		spdlog::error("seconds must be positive (got {})", seconds);
		return EXIT_FAILURE;
	}

	// The CLI already restricts --speed to the cadences the venue publishes;
	// this repeats the check because the enum is the authority on that list and
	// a caller reaching cmd_capture from anywhere else gets the same answer.
	const auto cadence = binance::from_string(speed);
	if (!cadence) {
		spdlog::error("unknown speed \"{}\": want {} or {}",
					  speed,
					  binance::depth_speed::every_100ms,
					  binance::depth_speed::every_1000ms);
		return EXIT_FAILURE;
	}

	auto [host, port, target] = binance::diff_depth_stream(symbol, *cadence);

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
