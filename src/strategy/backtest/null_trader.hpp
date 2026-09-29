#pragma once
#include "concepts.hpp"

namespace exchange::strategy::backtest {
/**
 * @brief A trader that submits nothing.
 *
 * Replays a capture through the whole harness with no order flow - which checks
 * the harness rather than a strategy, and is exactly what the invariant "the
 * engine's book equals the venue's published depth" wants driving it.
 */
struct null_trader {
	static std::size_t
	on_trades(std::span<const engine::trade> trades) noexcept {
		return trades.size();
	}

	static std::size_t
	on_outcomes(std::span<const engine::order_outcome> outcomes) noexcept {
		return outcomes.size();
	}

	static bool flush() noexcept { return true; }
};

static_assert(trader<null_trader>);
static_assert(!market_observer<null_trader>);
} // namespace exchange::strategy::backtest