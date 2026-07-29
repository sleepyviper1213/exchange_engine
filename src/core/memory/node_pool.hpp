#pragma once

#include "fwd.hpp"

#include <cassert>
#include <cstddef>
#include <vector>

namespace exchange::core::memory {

/**
 * @brief Single-threaded, growable pool of intrusively doubly-linked nodes
 *        addressed by a stable index.
 *
 * Each slot is a @c Node<T> carrying the payload plus @c next / @c prev links.
 * Handles are indices, not pointers: when the backing storage grows and
 * reallocates, every previously handed-out index stays valid, whereas a pointer
 * would dangle. That is what lets an OrderList thread its FIFO through pool
 * indices and still let the pool grow on demand.
 *
 * Index 0 is a reserved null sentinel (@c NO_NODE) — never handed out — so a
 * zero-initialized link reads as "no node" and callers can compare against
 * @c NO_NODE without a separate optional.
 *
 * @warning NOT thread-safe; one pool per owning thread (e.g. per book side).
 * @warning A reference from @c get() is invalidated by any subsequent
 *          @c allocate() that grows storage — re-fetch through the index rather
 *          than holding the reference across an allocation.
 *
 * @tparam T Payload type; must be default-constructible (slots are created
 *         empty and cleared on release).
 */
template <typename T>
class node_pool {
public:
	using Index = std::size_t;

	/// @brief Reserved null handle. Index 0 is never a live node.
	static constexpr Index NO_NODE = 0;

	/// @brief One pooled element: the payload plus its intrusive FIFO links.
	struct Node {
		Index next = NO_NODE; ///< next node in the owner's list, or NO_NODE
		Index prev = NO_NODE; ///< previous node in the owner's list, or NO_NODE
		T value{};          ///< the pooled payload
	};

	/// @brief Construct a pool, pre-reserving room for @p initial_capacity live
	///        nodes (plus the sentinel). Zero is fine — storage grows on
	///        demand.
	explicit node_pool(std::size_t initial_capacity = 0) {
		nodes_.reserve(initial_capacity + 1);
		nodes_.emplace_back(); // slot 0: the NO_NODE sentinel, never handed out
	}

	/// @brief Allocate a node, reusing a freed slot when one exists, otherwise
	///        appending fresh storage.
	/// @return The node's stable index (>= 1). Its links start at NO_NODE; its
	///         payload retains whatever a previous user left — assign before
	///         use.
	[[nodiscard]] Index allocate() {
		if (free_ != NO_NODE) {
			const Index idx = free_;
			free_           = nodes_[idx].next; // unlink from the free list
			Node &node      = nodes_[idx];
			node.next       = NO_NODE;
			node.prev       = NO_NODE;
			return idx;
		}
		nodes_.emplace_back();
		return nodes_.size() - 1;
	}

	/// @brief Return a node to the pool. Its index must not be used again until
	///        re-allocated. Double-free is caught by assert in hardened builds.
	void deallocate(Index idx) {
		assert(idx != NO_NODE && idx < nodes_.size() && "deallocate: bad index");
		Node &node = nodes_[idx];
		node.value =
			T{};   // drop payload state so it can't leak to the next user
		node.prev = NO_NODE;
		node.next =
			free_; // thread the free list through the (unused) next link
		free_ = idx;
	}

	/// @brief Access a node by index. Precondition: @p idx is a live handle.
	[[nodiscard]] Node &get(Index idx) noexcept {
		assert(idx != NO_NODE && idx < nodes_.size() && "get: bad index");
		return nodes_[idx];
	}

	[[nodiscard]] const Node &get(Index idx) const noexcept {
		assert(idx != NO_NODE && idx < nodes_.size() && "get: bad index");
		return nodes_[idx];
	}

	/// @brief Live-node capacity currently backed by storage (excludes
	/// sentinel).
	[[nodiscard]] std::size_t capacity() const noexcept {
		return nodes_.size() - 1;
	}

private:
	std::vector<Node> nodes_; ///< slot 0 is the NO_NODE sentinel
	Index free_ = NO_NODE;      ///< head of the free-slot list (linked via next)
};

} // namespace memory
