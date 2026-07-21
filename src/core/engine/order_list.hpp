#pragma once

#include "allocator/object_pool.hpp"
#include "types.hpp"

#include "resting_order.hpp"

namespace core::engine {

/// @brief Pool of resting-order nodes. Links are pool indices, not pointers, so
///        growing the backing storage never dangles them.
using OrderPool                  = allocator::ObjectPool<RestingOrder>;
using NodeIndex                  = OrderPool::index_type;
inline constexpr NodeIndex kNull = OrderPool::null_index;

/**
 * @brief A FIFO of resting orders held as intrusive pool nodes, plus the cached
 *        aggregate volume.
 *
 * Owns the head/tail invariant so callers never juggle prev/next by hand. The
 * pool is passed in rather than held, because the nodes live in one shared pool
 * and this type is embedded by value in every Level. Structural ops detach
 * nodes but do not free them: pool reclamation and the id->Location index are
 * the OrderBook's concern, so it frees the returned node.
 */
class OrderList {
public:
	[[nodiscard]] bool is_empty() const;

	/// @brief Append an allocated @p node carrying @p volume at the tail.
	void push_back(OrderPool &pool, NodeIndex node, Volume volume);

	/// @brief Detach the head node and return its index for the caller to free;
	///        deducts its remaining volume from the aggregate.
	NodeIndex pop_front(OrderPool &pool);

	/// @brief Splice @p node out for the caller to free; deducts its remaining
	///        volume from the aggregate.
	void unlink(OrderPool &pool, NodeIndex node);

	/// @brief The oldest resting order (fills first). Precondition: not empty.
	[[nodiscard]] RestingOrder &front(OrderPool &pool);

	/// @brief Aggregate resting volume across every node.
	[[nodiscard]] Volume volume() const noexcept;

	/// @brief Deduct @p amount from the front order's volume and the aggregate.
	void reduce_front(OrderPool &pool, Volume amount);

	/// @brief Collapse to a single node carrying @p volume: free every node
	/// after
	///        the head and overwrite the head's volume. Trailing nodes must
	///        carry no id->Location entries (depth-diff levels only).
	void reset_to_single(OrderPool &pool, Volume volume);

private:
	NodeIndex head      = kNull; ///< oldest order — fills first
	NodeIndex tail      = kNull; ///< newest order — appended here
	Volume total_volume = 0;     ///< sum of the resting nodes' volumes
};

} // namespace core::engine