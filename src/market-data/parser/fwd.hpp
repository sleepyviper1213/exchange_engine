#pragma once
// SIMD-accelerated parsing primitives shared by the market-data feed decoders
#include "market_data_export.hpp"

#include <cstdint>

namespace exchange::market_data::parser {

/// @brief Why a @c parse_fixed_point call rejected its input.
enum class parse_error : std::uint8_t;

} // namespace exchange::market_data::parser
