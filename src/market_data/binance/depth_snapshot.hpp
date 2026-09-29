#pragma once
// A parsed REST depth snapshot - the seed a diff stream is replayed onto.

#include "fwd.hpp"
#include "price_level.hpp"

#include <cstdint>
#include <vector>

namespace exchange::market_data::binance {

/**
 * @brief A parsed @c /api/v3/depth payload.
 *
 * Binance returns bids best-first (descending) and asks best-first (ascending)
 * - already in @c l2_book's preferred order.
 */
struct depth_snapshot {
	std::uint64_t lastUpdateId = 0;
	std::vector<PriceLevel> bids;
	std::vector<PriceLevel> asks;
};

} // namespace exchange::market_data::binance
