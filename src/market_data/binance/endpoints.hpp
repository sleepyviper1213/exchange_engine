#pragma once
// Where Binance publishes its market data, and under what names.
//
// This is venue knowledge, not transport knowledge: the host, the port, the
// `/ws/<stream>` grammar and the `@depth` / `@depth@100ms` suffixes are facts
// about Binance's market_data API, so they live in market_data alongside the
// decoder that reads what those endpoints return. transport/ only ever receives
// a resolved {host, port, target} and moves bytes.
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

#include "depth_speed.hpp"       // IWYU pragma: export
#include "fwd.hpp"
#include "market_data_export.hpp"
#include "venue/endpoint.hpp"    // IWYU pragma: export
#include "venue/environment.hpp" // IWYU pragma: export

#include <string>
#include <string_view>

namespace exchange::market_data::binance {

// The endpoint value types and the host table are the venue module's: both
// directions of traffic produce them, and a copy here would be a second place
// for the testnet switch to be got wrong. They are spelled with their owning
// namespace at every use below rather than aliased in here - the edge to
// `venue` should be legible where it is relied on.
// @see venue/endpoint.hpp, venue/binance/host.hpp

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
 * @param env Which deployment to stream from. Must match whatever the order
 *        path is pointed at - testnet keeps its own book. @see host_for
 * @return The endpoint to hand to @c transport::ws::capture.
 */
[[nodiscard]] MARKET_DATA_EXPORT venue::stream_endpoint
diff_depth_stream(std::string_view symbol,
				  depth_speed speed      = depth_speed::every_100ms,
				  venue::environment env = venue::environment::production);

/**
 * @brief The trade stream for @p symbol - the tape, one message per fill.
 *
 * The other half of what a venue publishes about a market. @ref
 * diff_depth_stream says what is *resting*; this says what actually *traded*,
 * and the two are independent subscriptions with independent delivery. Nothing
 * correlates them for you: a print carries no update id, so the only thing
 * relating a trade to a book state is the clock, and at the 100 ms depth
 * cadence that relation is coarser than the trade process it is being used to
 * explain.
 *
 * @param symbol Trading pair, in any case (e.g. @c BTCUSDT); stream names are
 *        lowercased for you.
 * @param env Which deployment to stream from. @see host_for
 * @return The endpoint to hand to @c transport::ws::capture.
 * @note This is the raw @c \@trade stream, one message per fill, not
 *       @c \@aggTrade. @see trade_message for why the finer feed is the one
 *       this tree decodes.
 */
[[nodiscard]] MARKET_DATA_EXPORT venue::stream_endpoint
trade_stream(std::string_view symbol,
			 venue::environment env = venue::environment::production);

/**
 * @brief The REST depth-snapshot endpoint for @p symbol - the seed book that a
 *        @ref diff_depth_stream is replayed onto.
 * @param symbol Trading pair (e.g. @c SOLUSDT); sent as given, uppercase.
 * @param limit Number of levels per side to request.
 * @return The endpoint to hand to @c transport::rest::get.
 */
[[nodiscard]] MARKET_DATA_EXPORT venue::http_endpoint depth_snapshot_endpoint(
	std::string_view symbol, int limit,
	venue::environment env = venue::environment::production);

// The exchangeInfo endpoint is not here. Its response is the trading grid,
// which order entry needs as much as depth reconstruction does, so the builder
// sits with the parser that reads it - @see venue/binance/exchange_info.hpp,
// exchange_info_endpoint.

} // namespace exchange::market_data::binance
