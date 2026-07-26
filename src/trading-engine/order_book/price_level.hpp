#pragma once

/**
 * @brief One aggregated price level from a Binance depth snapshot.
 *
 * Prices and sizes are stored as integers scaled by 10^decimals (no floating
 * point), so they drop straight into OrderBook's integral Price/Volume.
 */
struct PriceLevel {
	Price price;
	Volume volume;
};
