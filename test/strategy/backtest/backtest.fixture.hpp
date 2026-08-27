#pragma once
// Scaffolding shared by the backtest suites: a listing whose grids are all 1,
// and terse builders for the two normalised market_data payloads.

#include "market_data/normalised.hpp"
#include "event/command.hpp"
#include "execution/order_manager.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"
#include "symbol/symbol_spec.hpp"

// The sink these suites need already exists one directory up - one that records
// what it is given and can be told to refuse, which is exactly how a full SPSC
// queue looks from the producer side. Reaching for it beats copying it, and
// copying it here would be worse than merely redundant: `order_test` is one
// binary and these fixtures sit at global scope, so a second `recording_sink`
// with a different layout is an ODR violation the linker resolves by picking
// one of them. It presents as a sink pointer into the wrong object.
#include "../strategy.fixture.hpp" // IWYU pragma: export

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

// Only what this header's own declarations name and the strategy fixture has
// not already brought in. These sit at global scope - nothing here is nested
// inside `exchange`, so nothing is inherited from it.
using exchange::side_t;
using exchange::symbol_id_t;
using exchange::volume_t;

/// @brief A listing whose tick and lot are both 1, at scale 0.
///
/// The feed's scaled numbers and the engine's ticks and lots then coincide, so
/// a case can write prices and sizes as plain integers while still going
/// through the real @c symbol_spec conversions. A coarser grid is a different
/// test -
/// @c BacktestFillModel.RoundsThePublishedSizeDownToWholeLots is the one that
/// exercises it.
inline exchange::engine::symbol_spec unit_listing(symbol_id_t id = 0) {
	return exchange::engine::symbol_spec{id, "TEST", 0, 0, 1, 1, 100};
}

/// @brief One aggregated level, in the feed's scaled numbers.
inline exchange::market_data::book_level level(std::int64_t price,
											   std::int64_t qty) {
	return {.price = price, .qty = qty};
}

/// @brief A full-depth snapshot covering everything up to @p sequence.
inline exchange::market_data::book_snapshot
seed(exchange::market_data::sequence_t sequence,
	 std::initializer_list<exchange::market_data::book_level> bids,
	 std::initializer_list<exchange::market_data::book_level> asks) {
	return {.sequence   = sequence,
			.event_time = std::chrono::nanoseconds{0},
			.bids       = bids,
			.asks       = asks};
}

/// @brief One diff covering exactly @p sequence, stamped at @p stamp_ns.
inline exchange::market_data::depth_event
diff(exchange::market_data::sequence_t sequence, std::uint64_t stamp_ns,
	 std::initializer_list<exchange::market_data::book_level> bids,
	 std::initializer_list<exchange::market_data::book_level> asks) {
	return {.sequence   = {sequence, sequence},
			.event_time = std::chrono::nanoseconds{stamp_ns},
			.bids       = bids,
			.asks       = asks};
}

/// @brief Record a resting order of ours in @p orders, as a drain would.
inline void rest(exchange::engine::execution::order_manager &orders,
				 order_id_t id, side_t side, price_t price, quantity_t qty) {
	(void)orders.admit(exchange::engine::orders::order{
		.id        = id,
		.symbol_id = 0,
		.side      = side,
		.price     = price,
		.qty       = qty,
	});
}

/// @brief The order carried by the PLACE at @p index in a batch of commands.
inline const exchange::engine::orders::order &
placed(const std::vector<command> &batch, std::size_t index) {
	return batch[index].as_place();
}
