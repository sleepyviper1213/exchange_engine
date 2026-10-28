#pragma once
// Why a Binance depth payload could not be read.
//
// Split from the depth parsers so a caller that only reports the reason does
// not also compile the parser. The message each category prints as is its
// magic_enum name, specialised at the foot of this file.

#include "core/util/enum_format.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::market_data::binance {

using core::util::format_as;

/// @brief Why a depth payload was refused. Prints, and reads through
///        @c magic_enum::enum_name, as its category message.
enum class depth_error : std::uint8_t {
	invalid_json,
	missing_field,
	malformed_level,
	bad_number
};
} // namespace exchange::market_data::binance

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name(
	exchange::market_data::binance::depth_error value) noexcept {
	switch (value) {
		using enum exchange::market_data::binance::depth_error;
	case invalid_json: return "invalid JSON";
	case missing_field: return "missing or mistyped field";
	case malformed_level: return "level is not a [price, qty] pair";
	case bad_number: return "invalid number";
	}
	return default_tag;
}
