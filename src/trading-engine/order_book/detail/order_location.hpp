#pragma once
// Where a live order sits, which is `order_book`'s bookkeeping and nobody
// else's.
//
// It sat in the book's private section until now, which meant every consumer of
// order_book.hpp read the index's value type before reaching the interface it
// came for. `detail` says the same thing the access specifier did — this is not
// offered — without putting it in the way.

#include "../fwd.hpp"
#include "../price_level.hpp"
#include "resting_order.hpp"

namespace exchange::engine::detail {

/**
 * @brief Where a live order sits, for cancel by id.
 *
 * The node pointer is what makes cancel O(1), and the level pointer is what
 * makes it safe: both cells are pinned in their pools, so an entry recorded
 * when the order rested still names the same two objects however much the book
 * has changed since. Nothing has to be looked up to act on it.
 */
struct order_location {
	side_t side;
	price_level *level;
	resting_order *node;
};

} // namespace exchange::engine::detail
