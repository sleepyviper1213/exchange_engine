#pragma once

#include "core/util/enum_string.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace exchange {

/// @brief Fixed-point price in ticks; integral so equality/ordering are exact.
using price = std::uint64_t;
/// @brief Signed quantity; the L2 diff feed expresses reductions as negatives.
using quantity = std::int64_t;
/// @brief Stable identifier for a client order.
using order_id = std::uint64_t;

static_assert(!std::is_floating_point_v<price>,
			  "Price must not be floating point");
static_assert(std::is_unsigned_v<price>, "Price must be unsigned");

#define EXCHANGE_SIDE_LIST(X)                                                  \
	X(bid, "buy side; best price is the highest")                              \
	X(ask, "sell side; best price is the lowest")

enum class side : bool { EXCHANGE_ENUM_VALUES(EXCHANGE_SIDE_LIST) };

EXCHANGE_ENUM_NAME(side, to_string, EXCHANGE_SIDE_LIST)

/**
 * @brief The opposite side of @p s (bid <-> ask).
 * @param s A book side.
 * @return The opposing side.
 */
constexpr side opposed(side s) {
	return static_cast<side>(!static_cast<bool>(s));
}

} // namespace exchange