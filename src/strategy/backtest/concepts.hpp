#pragma once

#include "market_data/l2_book.hpp"
#include "order_book/outcome.hpp"

#include <concepts>
#include <span>

namespace exchange::strategy::backtest {
/**
 * @brief What the harness drives - the same three entry points a strategy host
 *        already has.
 *
 * @c strategy_engine satisfies this as written, which is the point: the thing
 * under test in a backtest should be the thing that runs in production,
 * composed the same way, not a special offline variant of it.
 */
template <class T>
concept trader = requires(T &t, std::span<const engine::trade> trades,
						  std::span<const engine::order_outcome> outcomes) {
	{ t.on_trades(trades) } -> std::convertible_to<std::size_t>;
	{ t.on_outcomes(outcomes) } -> std::convertible_to<std::size_t>;
	{ t.flush() } -> std::same_as<bool>;
};

/**
 * @brief A trader that also wants to look at the market - an optional hook.
 *
 * Detected with a concept and elided with @c if @c constexpr, the way
 * @c strategy_engine treats its own streams. It exists because a backtest can
 * offer something a live strategy host cannot: the venue's reconstructed depth,
 * in the same process, for free. A quoter needs it and a trade-driven strategy
 * does not, so it is opt-in rather than part of @c trader.
 */
template <class T>
concept market_observer =
	requires(T &t, const market_data::l2_book &replica, std::uint64_t now_ns) {
		t.on_market(replica, now_ns);
	};
} // namespace exchange::strategy::backtest