#pragma once
// The ids and time of a depthUpdate, without the levels.
//
// Its own header because the streaming apply path is the one caller, and that
// path exists precisely to avoid materialising levels - so it should not have
// to include the type that holds them. @see depth_update.hpp

#include "fwd.hpp"

#include <cstdint>

namespace exchange::market_data::binance {

/**
 * @brief The bookkeeping fields of a @c depthUpdate - everything except the
 * levels, which the streaming apply path writes straight to the book.
 *
 * Returned by @c apply_binance_depth_update / @c depth_parser::apply_update so
 * the caller still gets the update ids needed to sequence the managed local
 * order book (drop events already covered, detect gaps against lastUpdateId).
 */
struct depth_update_meta {
	std::uint64_t eventTime     = 0; ///< @c E - event time (ms since epoch)
	std::uint64_t firstUpdateId = 0; ///< @c U - first update id covered
	std::uint64_t finalUpdateId = 0; ///< @c u - last update id covered
};

} // namespace exchange::market_data::binance
