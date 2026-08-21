#pragma once
// The two inputs every per-command rule is measured against: a policy with one
// field tightened, and a batch that has consumed nothing yet.
//
// Both are `constexpr`, which is the point of the suites that use them - a rule
// that is a pure function of its inputs can be checked at compile time, and the
// arithmetic could not be while it was buried in a member function.

#include "../../gate/gate.fixture.hpp" // IWYU pragma: export
#include "risk_management/hooks/detail/screening.hpp"
#include "risk_management/limits.hpp"
#include "risk_management/hooks/system/trading_state.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstdint>

// A fixture at global scope cannot see these for free. @see testing.md
using exchange::volume_t;
using exchange::risk::hooks::system::trading_state;
using exchange::risk::hooks::detail::screen_state;

/// @brief Limits that refuse nothing, with the two size fields set.
[[nodiscard]] constexpr risk_limits sized(quantity_t max_qty,
										  std::int64_t max_notional) {
	risk_limits limits        = risk_limits{};
	limits.max_order_qty      = max_qty;
	limits.max_order_notional = max_notional;
	return limits;
}

/// @brief A batch that has consumed nothing yet, at @p net lots.
[[nodiscard]] constexpr screen_state fresh(volume_t net          = 0,
										   std::uint32_t headroom = 100) {
	return {.now              = at_ns(0),
			.state            = trading_state::NORMAL,
			.headroom         = headroom,
			.base_net         = net,
			.base_working_bid = 0,
			.base_working_ask = 0};
}
