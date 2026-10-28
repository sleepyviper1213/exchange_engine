#pragma once
#include "core/util/enum_format.hpp"

#include <cstdint>

namespace exchange::core::scaled {

using core::util::format_as;

/// @brief Why a @c parse_fixed_point call rejected its input. Prints, and
///        reads through @c magic_enum::enum_name, as a short human-readable
///        message - e.g. @c "empty number".
enum class parse_error : std::uint8_t {
	empty,
	negative_scale,
	invalid_char,
	no_digits,
	overflow,
};

} // namespace exchange::core::scaled

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name(
	exchange::core::scaled::parse_error value) noexcept {
	switch (value) {
		using enum exchange::core::scaled::parse_error;
	case empty: return "empty number";
	case negative_scale: return "negative scale";
	case invalid_char: return "invalid character in number";
	case no_digits: return "no digits in number";
	case overflow: return "number out of range";
	}
	return default_tag;
}
