#pragma once
// One parsed depthUpdate diff, levels and all.
// @see depth_update_meta.hpp for the ids-only shape the streaming path returns.

#include "fwd.hpp"
#include "price_level.hpp"

#include <cstdint>
#include <vector>

namespace exchange::market_data::binance {

/**
 * @brief One @c depthUpdate diff event from the WebSocket @c \<symbol\>@depth
 * stream.
 *
 * Unlike a snapshot, each level here is an @em absolute aggregated quantity,
 * not a delta: a level whose @c qty is 0 means "remove this price".
 * Replaying these onto a book seeded from a REST snapshot reconstructs the live
 * book - this is the managed-local-order-book procedure Binance documents.
 *
 * @see
 * https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams
 */
struct depth_update {
	std::uint64_t eventTime = 0; ///< @c E - event time (ms since epoch)
	std::uint64_t firstUpdateId =
		0;    ///< @c U - first update id covered by the event
	std::uint64_t finalUpdateId =
		0;    ///< @c u - last update id covered by the event
	std::vector<PriceLevel>
		bids; ///< @c b - bid_ levels, absolute qty (0 = remove)
	std::vector<PriceLevel>
		asks; ///< @c a - ask levels, absolute qty (0 = remove)
};

} // namespace exchange::market_data::binance
