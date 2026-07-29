#pragma once
#include "fwd.hpp"

namespace exchange::engine {

/**
 * @brief One execution produced by matching.
 * @note Trades always print at the resting (passive) order's price.
 */
struct Trade {
    order_id aggressor; ///< id of the incoming, aggressing order
    order_id resting;   ///< id of the passive order that was hit
    price price;       ///< execution price (the resting order's price)
    quantity volume;     ///< executed quantity
};

} // namespace exchange::engine
