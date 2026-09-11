#pragma once

#include <cstdint>

namespace exchange::core::scaled {

/// @brief Why a @c parse_fixed_point call rejected its input.
enum class parse_error : std::uint8_t;

} // namespace exchange::core::scaled
