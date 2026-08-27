#pragma once
// How often Binance publishes a depth diff.
//
// Split from endpoints.hpp so naming a stream's cadence does not require the
// URL builders. The X-macro list that generates the enumerator names and the
// wire spelling each maps to is part of the enum and travels with it.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <chrono>
#include <string_view>

namespace exchange::market_data::binance {

#define BINANCE_DEPTH_SPEED_LIST(X)                                            \
	X(every_1000ms, "1000ms") /* <symbol>@depth - one push per second      */  \
	X(every_100ms, "100ms")   /* <symbol>@depth@100ms - ten per second     */

/// @brief How often the diff-depth stream pushes an update (Binance spot).
enum class depth_speed : bool {
	EXCHANGE_ENUM_VALUES(BINANCE_DEPTH_SPEED_LIST)
};

/// @brief The cadence of @p s as Binance names it, e.g. @c "100ms" - and with
///        it the fmt hook, so a depth_speed prints as that cadence.
EXCHANGE_ENUM_LABEL(depth_speed, to_string, BINANCE_DEPTH_SPEED_LIST)

/// @brief The cadence Binance names @p text, or std::nullopt if it names none -
///        the exact inverse of to_string, generated from the same list.
EXCHANGE_ENUM_FROM_LABEL(depth_speed, from_string, BINANCE_DEPTH_SPEED_LIST)

/**
 * @brief How long @p speed leaves between pushes.
 *
 * The comment on the list above, made executable. Anything sizing a window
 * against the feed's cadence needs this number - a surveillance rule counting
 * events per window is meaningless if the window is narrower than the interval
 * between them - and deriving it from the wire spelling at each call site is
 * how two callers end up disagreeing about it.
 *
 * @note A ceiling on how often a diff *can* arrive, not a promise that one
 * will. The venue pushes on this cadence when the book has changed, so a quiet
 *       market sends fewer; nothing should treat this as a guaranteed rate.
 */
[[nodiscard]] constexpr std::chrono::milliseconds
interval(depth_speed speed) noexcept {
	return speed == depth_speed::every_100ms ? std::chrono::milliseconds{100}
											 : std::chrono::milliseconds{1000};
}

#undef BINANCE_DEPTH_SPEED_LIST

} // namespace exchange::market_data::binance
