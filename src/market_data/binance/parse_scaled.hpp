#pragma once
// Decimal text to a scaled integer - the one conversion every Binance payload
// needs and none of them should each own.

#include "market_data_export.hpp" // MARKET_DATA_EXPORT (generated)

#include <cstdint>
#include <expected>
#include <string_view>

namespace exchange {
namespace core::scaled {
// The underlying type is part of the declaration: parse_error.hpp defines it
// as `: std::uint8_t`, and an opaque re-declaration without one is a different
// type to the compiler, not a lighter spelling of the same one.
enum class parse_error : std::uint8_t;
}

namespace market_data::binance {

/**
 * @brief Convert a decimal string to an integer scaled by 10^decimals.
 *
 * Parses without floating point. Extra fractional digits are truncated; missing
 * ones are zero-padded. For example @c parse_scaled("153.45000000", 8) yields
 * @c 15345000000.
 * @param text The decimal string (optionally signed).
 * @param decimals Number of fractional digits to scale by; must be >= 0.
 * @return The scaled integer, or a @c core::scaled::parse_error on malformed
 * input.
 */
[[nodiscard]] MARKET_DATA_EXPORT
	std::expected<std::int64_t, core::scaled::parse_error>
	parse_scaled(std::string_view text, int decimals);

} // namespace market_data::binance
} // namespace exchange