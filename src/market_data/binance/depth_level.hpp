#pragma once
// One aggregated depth level as delivered on the Binance feed (scaffold).
//
// The parsed, scaled level the decoder actually produces is
// binance::PriceLevel in binance_depth.hpp; this header is reserved for a
// wire-shape depth-level view should the raw decoder need one. Not implemented
// yet; it only fixes the module's shape and namespace.

namespace exchange::market_data::binance {

// TODO: define the wire-level depth-level view, or fold into PriceLevel.

} // namespace exchange::market_data::binance
