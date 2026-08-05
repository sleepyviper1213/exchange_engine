#pragma once
#include "fwd.hpp"
#include "trading-engine/orders/types.hpp"
namespace exchange::engine {

/**
 * @brief One execution produced by matching.
 * @note Trades always print at the resting (passive) order's price.
 */
struct Trade {
    order_id_t aggressor; ///< id of the incoming, aggressing order
    order_id_t resting;   ///< id of the passive order that was hit
    price_t price;       ///< execution price (the resting order's price)
    quantity_t volume;     ///< executed quantity
};

} // namespace exchange::engine
