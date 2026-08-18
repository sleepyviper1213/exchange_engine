#pragma once
// SIMD-accelerated fixed-point decimal parsing.
//
// Exchange feeds quote prices and sizes as decimal *strings* ("153.45000000"),
// but the book stores them as integers scaled by 10^scale. Converting them is
// the per-level hot path of snapshot/diff decoding - one call for every price
// and every quantity in every frame. parse_fixed_point does it without floating
// point and without copying: it reads straight from the caller's bytes (which,
// on the feed path, already point into the JSON parser's buffer) and vectorises
// the digit run with SWAR - eight ASCII digits validated and folded per 64-bit
// word - falling back to a scalar tail. See fixed_point.cpp for the SSE variant.
#include "core/util/enum_string.hpp"
#include "market_data_export.hpp"

#include <cstdint>
#include <expected>
#include <string_view>

namespace exchange::market_data::parser {

#define PARSE_ERROR_LIST(X)                                                    \
	X(empty, "empty number")                                                   \
	X(negative_scale, "negative scale")                                        \
	X(invalid_char, "invalid character in number")                             \
	X(no_digits, "no digits in number")                                        \
	X(overflow, "number out of range")

/// @brief Why a @c parse_fixed_point call rejected its input.
enum class parse_error : std::uint8_t { EXCHANGE_ENUM_VALUES(PARSE_ERROR_LIST) };

/// @brief A short human-readable description of @p error (empty if out of range).
EXCHANGE_ENUM_LABEL(parse_error, message, PARSE_ERROR_LIST)

/**
 * @brief Parse a decimal string into an integer scaled by 10^@p scale.
 *
 * Pure integer arithmetic - no floating point. The optional sign, integer part,
 * and up to @p scale fractional digits are read; extra fractional digits are
 * validated and truncated, and a short fraction is zero-padded. For example
 * @c parse_fixed_point("153.45000000", 8) yields @c 15'345'000'000.
 *
 * The scan never reads past @p text, so any @c string_view is safe input: the
 * wide fold only fires while a full word still lies within the field, and a
 * scalar loop finishes the tail.
 *
 * @param text The decimal string (optionally signed).
 * @param scale Number of fractional digits to scale by; must be @c >= 0.
 * @return The scaled integer, or a @c parse_error on malformed input.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::expected<std::int64_t, parse_error>
parse_fixed_point(std::string_view text, int scale) noexcept;

} // namespace exchange::market_data::parser
