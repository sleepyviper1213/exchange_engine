#pragma once
// Reading the venue's depth cadence from a command line, in one place.
//
// `capture`, `live` and `serve` all take a `--speed` and all three had written
// out the same parse-and-complain block, down to the wording of the message and
// the two enumerators it names. A fourth command would have written it a fourth
// time, and a third cadence added to the enum would have needed three edits to
// stop the error text lying about the list.

#include "market_data/binance/depth_speed.hpp"

// Directly, not through core/logging.hpp: this header calls the free
// spdlog::error, and a header that names a symbol includes its declaration
// rather than relying on whatever its includer happened to pull in first.
#include <spdlog/spdlog.h>

#include <optional>
#include <string_view>

namespace exchange::app {

/**
 * @brief The cadence @p speed names, or nothing after logging what went wrong.
 *
 * @param speed The flag's text, e.g. @c "100ms".
 * @param flag How to name the option in the error - @c "--speed" for the
 *        commands that spell it that way, @c "speed" for a positional. Passed
 *        rather than hardcoded because the message is the only part the three
 *        call sites disagreed on, and an error naming a flag the user did not
 *        type is worse than a slightly generic one.
 * @return The cadence, or @c std::nullopt with an error already logged - so a
 *         caller returns @c EXIT_FAILURE and adds nothing.
 *
 * @note The check is repeated here even though the CLI restricts the option to
 *       the same values: the enum owns the list of cadences the venue
 *       publishes, and a caller reaching a command function from anywhere but
 *       the CLI gets the same answer. The error text is generated from the
 *       enumerators for the same reason - it cannot drift from the list it
 *       describes.
 */
[[nodiscard]] inline std::optional<market_data::binance::depth_speed>
cadence_from(std::string_view speed, std::string_view flag = "--speed") {
	namespace binance = market_data::binance;

	if (const auto cadence = binance::from_string(speed)) return cadence;

	spdlog::error("unknown {} \"{}\": want {} or {}",
				  flag,
				  speed,
				  binance::depth_speed::every_100ms,
				  binance::depth_speed::every_1000ms);
	return std::nullopt;
}

} // namespace exchange::app
