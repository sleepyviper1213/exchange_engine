#pragma once
// The synthetic flow every engine_partition benchmark drives, shared rather than
// copied.
//
// Sharing it is not tidiness here, it is the measurement. These benchmarks exist
// to be read against each other - what metrics cost a drain, what journalling
// costs a drain - and a difference between two arms only means what it claims if
// the flow through them is identical. Two copies of the generator is exactly how
// that stops being true without anybody noticing, since nothing fails when they
// drift; the numbers just quietly stop being comparable.

#include "trading-engine/event/command.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstddef>
#include <vector>

namespace exchange::bench {

/// @brief Commands per sampled drain, fixed so every arm times the same amount
///        of work per sample.
constexpr std::size_t PARTITION_BATCH = 64;

/**
 * @brief @p n commands that cross pairwise: an ask, then a bid that takes it.
 *
 * Each pair rests one order and consumes it whole, so the book returns to empty
 * and the run measures matching rather than the cost of an ever-deepening book.
 * @p base walks the price up between batches so successive drains do not all
 * touch one level.
 */
[[nodiscard]] inline std::vector<engine::event::command>
make_crossing_batch(std::size_t n, price_t base) {
	std::vector<engine::event::command> cmds;
	cmds.reserve(n);
	for (std::size_t i = 0; i < n; i += 2) {
		const price_t price      = base + static_cast<price_t>(i / 2);
		constexpr quantity_t qty = 10;
		cmds.push_back(engine::event::command::place(
			engine::orders::order{.id    = i + 1,
								  .side  = side_t::ask,
								  .price = price,
								  .qty   = qty}));
		cmds.push_back(engine::event::command::place(
			engine::orders::order{.id    = i + 2,
								  .side  = side_t::bid,
								  .price = price,
								  .qty   = qty}));
	}
	return cmds;
}

} // namespace exchange::bench
