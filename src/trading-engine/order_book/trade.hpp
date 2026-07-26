#pragma once
#include "fwd.hpp"

namespace exchange::engine {

/**
 * @brief One execution produced by matching.
 * @note Trades always print at the resting (passive) order's price.
 */
struct Trade {
    OrderId aggressor; ///< id of the incoming, aggressing order
    OrderId resting;   ///< id of the passive order that was hit
    Price price;       ///< execution price (the resting order's price)
    Volume volume;     ///< executed quantity
};

} // namespace exchange::engine
