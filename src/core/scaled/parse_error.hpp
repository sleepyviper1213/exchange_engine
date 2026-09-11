#pragma once
#include "core/util/enum_string.hpp"

#include <cstdint>

namespace exchange::core::scaled {

#define PARSE_ERROR_LIST(X)                                                    \
	X(empty, "empty number")                                                   \
	X(negative_scale, "negative scale")                                        \
	X(invalid_char, "invalid character in number")                             \
	X(no_digits, "no digits in number")                                        \
	X(overflow, "number out of range")

/// @brief Why a @c parse_fixed_point call rejected its input.
enum class parse_error : std::uint8_t {
	EXCHANGE_ENUM_VALUES(PARSE_ERROR_LIST)
};

/// @brief A short human-readable description of @p error (empty if out of
/// range).
EXCHANGE_ENUM_LABEL(parse_error, message, PARSE_ERROR_LIST)

#undef PARSE_ERROR_LIST
} // namespace exchange::core::scaled