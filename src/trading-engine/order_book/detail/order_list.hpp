#pragma once

#include "resting_order.hpp"
#include "core/memory/node_pool.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <cstdint>

namespace exchange::engine::detail {

/// @brief Pool of resting-order nodes. Links are pool indices, not pointers, so
///        growing the backing storage never dangles them.
using order_pool                  = core::memory::node_pool<resting_order>;
using node_index                  = order_pool::Index;
inline constexpr node_index NO_NODE = order_pool::NO_NODE;

/**
 * @brief A FIFO of resting orders held as intrusive pool nodes, plus the cached
 *        aggregate qty and order count.
 *
 * Owns the head/tail invariant so callers never juggle prev/next by hand. The
 * pool is passed in rather than held, because the nodes live in one shared pool
 * and this type is embedded by value in every Level. Structural ops detach
 * nodes but do not free them: pool reclamation and the id->Location index are
 * the OrderBook's concern, so it frees the returned node.
 *
 * The aggregate qty and count are maintained incrementally rather than
 * walked on demand: can_fully_fill asks every crossing level for its total, so
 * a summing implementation would make a fill-or-kill check O(resting orders)
 * rather than O(levels).
 */
class order_list {
public:
	[[nodiscard]] bool is_empty() const;

	/// @brief Append an allocated @p node carrying @p qty at the tail.
	void push_back(order_pool &pool, node_index node, quantity volume);

	/// @brief Detach the head node and return its index for the caller to free;
	///        deducts its remaining qty from the aggregate.
	node_index pop_front(order_pool &pool);

	/// @brief Splice @p node out for the caller to free; deducts its remaining
	///        qty from the aggregate.
	void unlink(order_pool &pool, node_index node);

	/// @brief The oldest resting order (fills first). Precondition: not empty.
	[[nodiscard]] resting_order &front(order_pool &pool);

	/// @brief The most recently appended node, or @c NO_NODE when empty.
	///
	/// This is the node a just-completed @c push_back added, which is how the
	/// book records where a newly rested order landed without threading an
	/// out-parameter back through @c book_side::insert.
	[[nodiscard]] node_index back() const noexcept;

	/// @brief Aggregate resting qty across every node. O(1).
	[[nodiscard]] quantity aggregate_resting_volume() const noexcept;

	/// @brief Number of orders resting in the FIFO. O(1).
	[[nodiscard]] std::size_t size() const noexcept;

	/// @brief Deduct @p amount from the front order's qty and the aggregate.
	void reduce_front(order_pool &pool, quantity amount);

	/// @brief Collapse to a single node carrying @p qty: free every node
	/// after
	///        the head and overwrite the head's qty. Trailing nodes must
	///        carry no id->Location entries (depth-diff levels only).
	void reset_to_single(order_pool &pool, quantity volume);

private:
	node_index head       = NO_NODE; ///< oldest order — fills first
	node_index tail       = NO_NODE; ///< newest order — appended here
	quantity total_volume = 0;       ///< sum of the resting nodes' volumes
	std::uint32_t count   = 0;       ///< number of resting nodes
};

} // namespace exchange::engine::detail
