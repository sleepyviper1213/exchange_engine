#pragma once
// Where Binance publishes its depth, and under what names.
//
// This is venue knowledge, not transport knowledge: the host, the port, the
// `/ws/<stream>` grammar and the `@depth` / `@depth@100ms` suffixes are facts
// about Binance's market-data API, so they live in market-data alongside the
// decoder that reads what those endpoints return. transport/ only ever receives
// a resolved {host, port, target} and moves bytes.
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

#include "core/util/enum_string.hpp"
#include "market_data_export.hpp"
#include "fwd.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace exchange::market_data::binance {

#define BINANCE_DEPTH_SPEED_LIST(X)                                            \
	X(every_1000ms, "1000ms") /* <symbol>@depth — one push per second      */   \
	X(every_100ms, "100ms")   /* <symbol>@depth@100ms — ten per second     */

/// @brief How often the diff-depth stream pushes an update (Binance spot).
enum class depth_speed : std::uint8_t {
	EXCHANGE_ENUM_VALUES(BINANCE_DEPTH_SPEED_LIST)
};

/// @brief The cadence of @p s as Binance names it, e.g. @c "100ms" — and with
///        it the fmt hook, so a depth_speed prints as that cadence.
EXCHANGE_ENUM_LABEL(depth_speed, to_string, BINANCE_DEPTH_SPEED_LIST)

/// @brief A resolved WebSocket endpoint: everything transport needs to open a
///        connection, and nothing about what the frames mean.
struct stream_endpoint {
	std::string host;   ///< TLS host, also the SNI
	std::string port;   ///< TLS port
	std::string target; ///< stream path, e.g. @c /ws/solusdt@depth@100ms
};

/// @brief A resolved HTTPS endpoint for a one-shot REST GET.
struct http_endpoint {
	std::string host;   ///< TLS host, also the SNI + Host header
	std::string target; ///< request path with query
};

/**
 * @brief The diff-depth (@c depthUpdate) stream for @p symbol.
 *
 * This is the feed that drives local-order-book reconstruction: each frame
 * carries the new @em absolute aggregate size for the levels it touches, which
 * is exactly what @c l2_book::set_level consumes. Seed an @c l2_book from a
 * @ref depth_snapshot REST payload, then apply these frames to keep it live —
 * the managed-local-order-book procedure Binance documents.
 *
 * @param symbol Trading pair, in any case (e.g. @c SOLUSDT); stream names are
 *        lowercased for you.
 * @param speed Push cadence; @c every_100ms unless the caller says otherwise.
 * @return The endpoint to hand to @c transport::ws::capture.
 */
[[nodiscard]] MARKET_DATA_EXPORT stream_endpoint
diff_depth_stream(std::string_view symbol,
				  depth_speed speed = depth_speed::every_100ms);

/**
 * @brief The REST depth-snapshot endpoint for @p symbol — the seed book that a
 *        @ref diff_depth_stream is replayed onto.
 * @param symbol Trading pair (e.g. @c SOLUSDT); sent as given, uppercase.
 * @param limit Number of levels per side to request.
 * @return The endpoint to hand to @c transport::rest::get.
 */
[[nodiscard]] MARKET_DATA_EXPORT http_endpoint
depth_snapshot(std::string_view symbol, int limit);

} // namespace exchange::market_data::binance
