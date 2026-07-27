#pragma once

#include "detail/freelist/local.hpp"
#include "fwd.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#ifdef __linux__
#include <numa.h> // numa_alloc_onnode, numa_free, numa_available
#endif

namespace exchange::core::memory {
/**
 * @brief Bump-allocated arena backed by a single large pool plus a free list.
 *
 * The bump/free-list core is portable; only how the pool is acquired differs by
 * platform. init(size) takes node-agnostic memory from the system allocator and
 * works everywhere; init(size, node) (Linux + libnuma only) binds the pool to a
 * NUMA node's local memory so hot allocations stay near the thread touching
 * them, falling back to the portable path when NUMA is unavailable.
 *
 * Every request is rounded up to a power-of-two size class (at least
 * sizeof(void*), so a freed block can host the intrusive free-list node), and
 * each class owns its own free list. allocate() serves that class's list first
 * (O(1) reuse of returned blocks), then bumps a monotonically increasing
 * offset. Recycling per class is what makes reuse sound: a block handed back
 * may only satisfy a request that fits it. A single shared list would hand a
 * small freed block to a larger request, overlapping it with live storage.
 *
 * This arena is thread-confined: its owner performs every allocate() and
 * deallocate(). Cross-thread returns must be routed to that owner before they
 * touch the per-class local free lists.
 */
class alignas(std::hardware_destructive_interference_size) arena {
public:
	arena() noexcept = default;

	arena(const arena &) = delete;

	arena &operator=(const arena &) = delete;

	/// @brief Bind this arena to a @p size-byte pool from the system allocator.
	///        Portable — available on every platform.
	CORE_AUTOTEST_EXPORT void init(std::size_t size);

#ifdef __linux__
	/// @brief Bind this arena to @p node with a @p size-byte node-local pool.
	///        Falls back to the portable path when libnuma reports no NUMA.
	CORE_AUTOTEST_EXPORT void init(std::size_t size, int node);
#endif

	CORE_AUTOTEST_EXPORT ~arena();

	/// @brief Try to satisfy a @p bytes / @p align request: the matching size
	///        class's free list first, then bump the pointer.
	/// @return Pointer to the block, or nullptr if this arena cannot serve it.
	[[nodiscard]] CORE_AUTOTEST_EXPORT void *
	allocate(std::size_t bytes, std::align_val_t align) noexcept;

	/// @brief Return @p ptr to its size class's free list for reuse.
	/// @note @p bytes and @p align must match the allocate() call that produced
	///       @p ptr — they select the class the block goes back to.
	CORE_AUTOTEST_EXPORT void deallocate(void *ptr, std::size_t bytes,
										 std::align_val_t align) noexcept;

private:
	/// @brief Power-of-two block actually handed out for a request. At least
	///        sizeof(void*) so a freed block can hold the free-list node, and
	///        at least @p align so the class's alignment guarantee covers it.
	static std::size_t block_size(std::size_t bytes,
								  std::align_val_t align) noexcept;

	/// @brief Free-list index for a power-of-two block size.
	static std::size_t size_class(std::size_t block) noexcept;

	static constexpr std::align_val_t kPoolAlign{
		std::hardware_destructive_interference_size};
	static constexpr std::size_t kMinBlock    = free_list::kMinBlockBytes;
	static constexpr std::size_t kSizeClasses = 64; ///< one per power of two

	std::uint8_t *memory_pool_{nullptr};
	std::size_t pool_size_{0};
	std::atomic<std::size_t> allocated_{0};
	std::array<free_list, kSizeClasses> free_lists_{};
	bool numa_backed_{false};
};
} // namespace memory
