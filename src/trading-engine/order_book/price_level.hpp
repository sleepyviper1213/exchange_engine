#pragma once

#include "core/types.hpp"

namespace exchange::engine {

/**
 * @brief One aggregated price level from a depth snapshot.
 *
 * Prices and sizes are stored as integers scaled by 10^decimals (no floating
 * point), so they drop straight into the order book's integral Price/Volume.
 */
struct PriceLevel {
	price price;
	quantity volume;
};

} // namespace exchange::engine
