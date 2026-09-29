#pragma once
// What a failed depth parse reports back - the category plus where it happened.
//
// Split from the parsers for the reason depth_error.hpp is split from this: a
// caller that only reports the reason should not also compile simdjson.

#include "depth_error.hpp"        // IWYU pragma: export
#include "fwd.hpp"
#include "market_data_export.hpp" // MARKET_DATA_EXPORT (generated)

#include <cstdint>
#include <string>
#include <string_view>

namespace exchange::market_data::binance {

/**
 * @brief A depth-parse failure: a category plus optional static context.
 *
 * @c context is always a static string (an offending field name, simdjson's own
 * message, or the numeric @c parse_error message) - never a view into the
 * parsed buffer, so it outlives the parse call. @c line is the 1-based line in
 * a JSONL feed, or 0 when not applicable.
 */
struct depth_parse_error {
	depth_error code;
	std::string_view
		context{}; ///< Default-initialised so a brace-init may
				   ///< name only the code. @see feed_status::detail
	std::uint32_t line = 0;
};

/// @brief Render a @c depth_parse_error as "[line L: ][context: ]category".
[[nodiscard]] MARKET_DATA_EXPORT std::string
message(const depth_parse_error &error);

} // namespace exchange::market_data::binance
