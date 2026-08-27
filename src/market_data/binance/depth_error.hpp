#pragma once
// Why a Binance depth payload could not be read.
//
// Split from binance_depth.hpp so a caller that only reports the reason does
// not also compile the parser. The X-macro list that generates the enumerator
// names and their messages is part of the enum and travels with it.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::market_data::binance {

#define DEPTH_ERROR_LIST(X)                                                    \
	X(invalid_json, "invalid JSON")                                            \
	X(missing_field, "missing or mistyped field")                              \
	X(malformed_level, "level is not a [price, qty] pair")                     \
	X(bad_number, "invalid number")

enum class depth_error : std::uint8_t {
	EXCHANGE_ENUM_VALUES(DEPTH_ERROR_LIST)
};

/// @brief The category message for a @c depth_error (empty view if out of
/// range).
EXCHANGE_ENUM_LABEL(depth_error, message, DEPTH_ERROR_LIST)


#undef DEPTH_ERROR_LIST
} // namespace exchange::market_data::binance
