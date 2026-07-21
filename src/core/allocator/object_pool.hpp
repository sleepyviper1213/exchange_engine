#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace core::allocator {

/**
 * @brief Fixed-capacity pool of intrusive nodes with O(1) allocate/deallocate.
 *
 * Reuses freed slots via a free list. Capacity is reserved up front and never
 * grown, because growing would reallocate the backing vector and invalidate
 * every reference handed out by get().
 * @tparam T Payload type; must be standard layout.
 */
template<typename T>
    requires(std::is_standard_layout_v<T>)
class ObjectPool {
public:

    /// Index type used to address nodes; -1 (null_index) means "none".
    using index_type = std::int32_t;

    /// Sentinel returned/stored where no node exists.
    static constexpr index_type null_index = -1;

	/**
	 * @brief Intrusive list node stored inside an ObjectPool.
	 *
	 * Links are pool indices (not pointers), so growing the backing storage never
	 * dangles them. A link of ObjectPool<T>::null_index marks "no neighbour".
	 * @tparam T Payload type held by the node.
	 */
	struct [[nodiscard]] Node {
		T value;
		index_type next = null_index;
		index_type prev = null_index;
	};

    /**
     * @brief Construct a pool that can hold up to @p cap live nodes.
     * @param cap Maximum number of nodes; storage is reserved immediately.
     */
    explicit constexpr ObjectPool(std::size_t cap = 524288) {
        nodes_.reserve(cap);
        free_list_.reserve(cap);
    }

    /**
     * @brief Allocate a node, constructing its value in place.
     * @param args Arguments forwarded to T's constructor.
     * @return The index of the freshly allocated node.
     */
    template<typename... Args>
    index_type allocate(Args &&... args) {
        index_type index;
        if (!free_list_.empty()) {
            index = free_list_.back();
            free_list_.pop_back();
            std::destroy_at(&nodes_[index].value);
        } else {
            // Growing past capacity would reallocate and dangle every reference
            // handed out by get(); the pool is fixed-size.
            assert(nodes_.size() < nodes_.capacity() && "ObjectPool exhausted");
            nodes_.emplace_back();
            index = static_cast<index_type>(nodes_.size() - 1);
        }

        nodes_[index].value = T{std::forward<Args>(args)...};
        nodes_[index].next = null_index;
        nodes_[index].prev = null_index;
        return index;
    }

    /// @brief Number of currently live (allocated and not freed) nodes.
    [[nodiscard]] std::size_t size() const {
        return nodes_.size() - free_list_.size();
    }

    /**
     * @brief Return a node's slot to the free list for reuse.
     * @param index Index previously returned by allocate().
     */
    void deallocate(index_type index) { free_list_.push_back(index); }

    /// @brief Access the node at @p index.
    template<typename Self>
    auto &&get(this Self&& self, index_type index) {
	    return std::forward<Self>(self).nodes_[index];
    }

private:
    std::vector<Node> nodes_;
    std::vector<index_type> free_list_;
};

} // namespace core::allocator
