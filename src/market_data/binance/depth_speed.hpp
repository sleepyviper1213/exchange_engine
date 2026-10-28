#pragma once
// How often Binance publishes a depth diff.
//
// Split from endpoints.hpp so naming a stream's cadence does not require the
// URL builders. The wire spelling each cadence maps to is its magic_enum name,
// specialised at the foot of this file.

#include "core/util/enum_format.hpp"

#include <chrono>
#include <cstdint>

namespace exchange::market_data::binance {

using core::util::format_as;

/// @brief How often the diff-depth stream pushes an update (Binance spot).
///        Prints as the cadence Binance names it, e.g. @c "100ms", and
///        @c magic_enum::enum_cast parses exactly that spelling back - the
///        identifier is not accepted.
/// @note Not @c bool-backed: magic_enum 0.9.7's string @c enum_cast returns
///       the @c true enumerator for *any* text on a @c bool enum, so
///       @c "10ms" would parse as @c every_100ms.
enum class depth_speed : std::uint8_t {
	every_1000ms, ///< <symbol>@depth - one push per second
	every_100ms,  ///< <symbol>@depth@100ms - ten per second
};

/**
 * @brief How long @p speed leaves between pushes.
 *
 * The enumerator comments above, made executable. Anything sizing a window
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
	using namespace std::chrono_literals;
	return speed == depth_speed::every_100ms ? 100ms : 1000ms;
}
} // namespace exchange::market_data::binance

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name(
	exchange::market_data::binance::depth_speed value) noexcept {
	switch (value) {
		using enum exchange::market_data::binance::depth_speed;
	case every_1000ms: return "1000ms";
	case every_100ms: return "100ms";
	}
	return default_tag;
}