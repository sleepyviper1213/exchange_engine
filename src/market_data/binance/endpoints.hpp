#pragma once
// Where Binance publishes its depth, and under what names.
//
// This is venue knowledge, not transport knowledge: the host, the port, the
// `/ws/<stream>` grammar and the `@depth` / `@depth@100ms` suffixes are facts
// about Binance's market_data API, so they live in market_data alongside the
// decoder that reads what those endpoints return. transport/ only ever receives
// a resolved {host, port, target} and moves bytes.
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

#include "depth_speed.hpp" // IWYU pragma: export
#include "fwd.hpp"
#include "market_data_export.hpp"

#include <string>
#include <string_view>

namespace exchange::market_data::binance {

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
 * @ref depth_snapshot REST payload, then apply these frames to keep it live -
 * the managed-local-order-book procedure Binance documents.
 *
 * @param symbol Trading pair, in any case (e.g. @c SOLUSDT); stream names are
 *        lowercased for you.
 * @param speed Push cadence; @c every_100ms unless the caller says otherwise.
 * @return The endpoint to hand to @c transport::ws::capture.
 */
[[nodiscard]] MARKET_DATA_EXPORT stream_endpoint diff_depth_stream(
	std::string_view symbol, depth_speed speed = depth_speed::every_100ms);

/**
 * @brief The REST depth-snapshot endpoint for @p symbol - the seed book that a
 *        @ref diff_depth_stream is replayed onto.
 * @param symbol Trading pair (e.g. @c SOLUSDT); sent as given, uppercase.
 * @param limit Number of levels per side to request.
 * @return The endpoint to hand to @c transport::rest::get.
 */
[[nodiscard]] MARKET_DATA_EXPORT http_endpoint
depth_snapshot(std::string_view symbol, int limit);

/**
 * @brief The REST endpoint describing @p symbol's trading rules.
 *
 * Where the venue publishes the price and size grid - @c PRICE_FILTER.tickSize
 * and @c LOT_SIZE.stepSize - which is reference data every other number in a run
 * is quantised against. Worth fetching rather than configuring: the grid differs
 * per listing and getting it wrong is silent, because surplus precision is
 * *truncated* on the way in rather than refused. A step configured coarser than
 * the venue's rounds small levels to nothing and reports a healthy feed.
 *
 * @param symbol Trading pair (e.g. @c SOLUSDT); sent as given, uppercase.
 * @return The endpoint to hand to @c transport::rest::get.
 *
 * @note Scoped to one symbol on purpose. The unfiltered response describes every
 *       listing on the venue and is megabytes; the single-symbol form is one
 *       object and costs a fraction of the rate-limit weight.
 */
[[nodiscard]] MARKET_DATA_EXPORT http_endpoint
exchange_info(std::string_view symbol);

} // namespace exchange::market_data::binance
