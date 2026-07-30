#pragma once

// NUMA-aware arena allocator: each of up to kMaxNodes NUMA nodes owns a
// bump-allocated memory pool sitting in that node's local memory, plus a
// lock-free free list of returned blocks for O(1) reuse. Allocation routes to
// the arena for the caller's current NUMA node, so hot allocations stay on the
// same node as the thread touching them.
#if defined(__linux__) && defined(ORDER_BOOK_WITH_NUMA)
#include "arena.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace exchange::core::memory {

/**
 * @brief NUMA-aware allocator that serves memory from the caller's local node.
 */
class numa_arena_allocator {
public:
	static constexpr std::size_t kMaxNodes = 8; ///< supported NUMA nodes

	/// @brief Create one @p arena_size-byte arena bound to each NUMA node.
	explicit numa_arena_allocator(std::size_t arena_size);

	numa_arena_allocator(const numa_arena_allocator &)            = delete;
	numa_arena_allocator &operator=(const numa_arena_allocator &) = delete;

	/// @brief Allocate from the current node's arena, falling back to malloc.
	[[nodiscard]] void *alloc(std::size_t bytes,
							  std::align_val_t align) noexcept;

	/// @brief Return @p ptr to the current node's arena free list. @p bytes and
	///        @p align must match the alloc() call that produced @p ptr; they
	///        pick the size class the block is recycled into.
	///
	/// @note Like the Rust original, this cannot tell an arena pointer from a
	///       fallback (system) pointer, and pushes either onto the free list.
	///       That is only sound when every allocation comes from an arena; if
	///       the fallback path can fire, track provenance before adopting this.
	void dealloc(void *ptr, std::size_t bytes, std::align_val_t align) noexcept;

private:
	/// @brief NUMA node of the CPU currently running this thread, clamped to
	///        the arena range.
	[[nodiscard]] std::size_t current_node() const noexcept;

	arena arenas_[kMaxNodes]{};
	std::atomic<std::uint32_t> current_node_hint_{0};
};

} // namespace exchange::core::memory
#endif