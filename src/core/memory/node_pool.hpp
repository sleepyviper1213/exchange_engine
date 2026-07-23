#pragma once

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace memory {

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
 * Index 0 is a reserved null sentinel (@c kNull) — never handed out — so a
 * zero-initialized link reads as "no node" and callers can compare against
 * @c kNull without a separate optional.
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
class NodePool {
public:
	using Index = std::size_t;

	/// @brief Reserved null handle. Index 0 is never a live node.
	static constexpr Index kNull = 0;

	/// @brief One pooled element: the payload plus its intrusive FIFO links.
	struct Node {
		Index next = kNull; ///< next node in the owner's list, or kNull
		Index prev = kNull; ///< previous node in the owner's list, or kNull
		T value{};          ///< the pooled payload
	};

	/// @brief Construct a pool, pre-reserving room for @p initial_capacity live
	///        nodes (plus the sentinel). Zero is fine — storage grows on demand.
	explicit NodePool(std::size_t initial_capacity = 0) {
		nodes_.reserve(initial_capacity + 1);
		nodes_.emplace_back(); // slot 0: the kNull sentinel, never handed out
	}

	/// @brief Allocate a node, reusing a freed slot when one exists, otherwise
	///        appending fresh storage.
	/// @return The node's stable index (>= 1). Its links start at kNull; its
	///         payload retains whatever a previous user left — assign before use.
	[[nodiscard]] Index allocate() {
		if (free_ != kNull) {
			const Index idx = free_;
			free_           = nodes_[idx].next; // unlink from the free list
			Node &node      = nodes_[idx];
			node.next       = kNull;
			node.prev       = kNull;
			return idx;
		}
		nodes_.emplace_back();
		return nodes_.size() - 1;
	}

	/// @brief Return a node to the pool. Its index must not be used again until
	///        re-allocated. Double-free is caught by assert in hardened builds.
	void deallocate(Index idx) {
		assert(idx != kNull && idx < nodes_.size() && "deallocate: bad index");
		Node &node = nodes_[idx];
		node.value = T{};    // drop payload state so it can't leak to the next user
		node.prev  = kNull;
		node.next  = free_;  // thread the free list through the (unused) next link
		free_      = idx;
	}

	/// @brief Access a node by index. Precondition: @p idx is a live handle.
	[[nodiscard]] Node &get(Index idx) noexcept {
		assert(idx != kNull && idx < nodes_.size() && "get: bad index");
		return nodes_[idx];
	}
	[[nodiscard]] const Node &get(Index idx) const noexcept {
		assert(idx != kNull && idx < nodes_.size() && "get: bad index");
		return nodes_[idx];
	}

	/// @brief Live-node capacity currently backed by storage (excludes sentinel).
	[[nodiscard]] std::size_t capacity() const noexcept {
		return nodes_.size() - 1;
	}

private:
	std::vector<Node> nodes_; ///< slot 0 is the kNull sentinel
	Index free_ = kNull;      ///< head of the free-slot list (linked via next)
};

} // namespace memory
