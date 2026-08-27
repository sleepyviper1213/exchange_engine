#pragma once
// Two listings shaped like the real thing, shared by every symbol test: one
// equity, one crypto pair. They differ in every dimension that matters - scale,
// tick, lot, reference price - so a check that passes for both is not passing
// by coincidence.

#include "symbol.hpp"


using exchange::engine::symbol_spec;

/// @brief A US-equity-shaped listing: 2 decimals, penny tick, whole shares,
///        $50 reference, ±20% band.
inline symbol_spec equity() { return symbol_spec{1, "ACME", 2, 0, 1, 1, 5000, 2000}; }

/// @brief A crypto-shaped listing: 8 decimals on both sides, 0.01 price tick,
///        0.00001 lot, $60,000 reference, ±20% band.
inline symbol_spec crypto() {
	return symbol_spec{2, "BTCUSDT", 8, 8, 1'000'000, 1'000, 6'000'000'000'000,
					   2000};
}

