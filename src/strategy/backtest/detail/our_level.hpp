#pragma once
// One price of ours, with everything the fill model believes rests there.
//
// A private nested struct until now, which in a class template means the public
// header — a template has no private section a reader cannot see. `detail` is
// what says this is bookkeeping rather than interface.

#include "../fwd.hpp"
#include "trading-engine/orders/types.hpp"

namespace exchange::strategy::backtest::detail {

/// @brief One price of ours, with everything resting there.
struct our_level {
	price_t price;
	volume_t lots;
};

} // namespace exchange::strategy::backtest::detail
