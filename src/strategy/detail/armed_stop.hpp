#pragma once
// What `stop` holds back until its trigger price prints.
//
// A private nested struct until now, which in a class template means the public
// header — a template has no private section a reader cannot see. `detail` is
// what says this is bookkeeping rather than interface.

#include "../fwd.hpp"
#include "trading-engine/orders/order.hpp"

namespace exchange::strategy::detail {

/// @brief One armed stop: the order to release, and whether the slot is in use.
struct armed_stop {
	engine::orders::order resting;
	bool active;
};

} // namespace exchange::strategy::detail
