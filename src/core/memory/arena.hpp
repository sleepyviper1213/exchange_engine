#pragma once

#include "memory/free_list.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(__linux__)
#include <numa.h> // numa_alloc_onnode, numa_free, numa_available
#endif

namespace memory {

/// Cache-line size used to keep each arena off its neighbours' coherency line.
/// Fixed rather than std::hardware_destructive_interference_size so the value
/// can't vary with -mtune (and to avoid -Winterference-size under -Werror);
/// 64 bytes matches every x86-64 and AArch64 target this engine runs on.
inline constexpr std::size_t kCacheLineBytes = 64;

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
 * (O(1) reuse of returned blocks), then bumps a monotonically increasing offset.
 * Recycling per class is what makes reuse sound: a block handed back may only
 * satisfy a request that fits it. A single shared list would hand a small freed
 * block to a larger request, overlapping it with live storage.
 *
 * The arena never returns memory to the OS during its life, so handed-out
 * addresses are stable — which is also what makes the intrusive FreeList's ABA
 * assumption hold.
 */
struct alignas(kCacheLineBytes) arena {
	arena() noexcept                = default;
	arena(const arena &)            = delete;
	arena &operator=(const arena &) = delete;

	/// @brief Bind this arena to a @p size-byte pool from the system allocator.
	///        Portable — available on every platform.
	void init(std::size_t size) {
		pool_size_   = size;
		numa_backed_ = false;
		memory_pool_ = static_cast<std::uint8_t *>(
			::operator new(size, kPoolAlign, std::nothrow));
		if (memory_pool_ == nullptr) std::abort();
	}

#if defined(__linux__)
	/// @brief Bind this arena to @p node with a @p size-byte node-local pool.
	///        Falls back to the portable path when libnuma reports no NUMA.
	void init(std::size_t size, int node) {
		if (::numa_available() < 0) {
			init(size);
			return;
		}
		pool_size_   = size;
		numa_backed_ = true;
		memory_pool_ =
			static_cast<std::uint8_t *>(::numa_alloc_onnode(size, node));
		if (memory_pool_ == nullptr) std::abort();
	}
#endif

	~arena() {
		if (memory_pool_ == nullptr) return;
#if defined(__linux__)
		if (numa_backed_) {
			::numa_free(memory_pool_, pool_size_);
			return;
		}
#endif
		::operator delete(memory_pool_, kPoolAlign);
	}

	/// @brief Try to satisfy a @p bytes / @p align request: the matching size
	///        class's free list first, then bump the pointer.
	/// @return Pointer to the block, or nullptr if this arena cannot serve it.
	[[nodiscard]] void *allocate(std::size_t bytes,
								 std::align_val_t align) noexcept {
		if (bytes > pool_size_) return nullptr; // never satisfiable here
		const std::size_t need =
			block_size(bytes, static_cast<std::size_t>(align));

		// O(1) fast path: reuse a block returned by this same size class. Any
		// block in the class is `need` bytes and at least as aligned as any
		// request the class accepts, so the reuse is a genuine fit.
		if (void *reused = free_lists_[size_class(need)].pop()) return reused;

		// Bump path. Align the *offset* (not just the size) so every returned
		// block satisfies its alignment; the pool base is over-aligned to
		// kPoolAlign, so an aligned offset yields an aligned pointer. Blocks
		// below a cache line align to their own (power-of-two) size, which is
		// >= the requested alignment; larger ones align to the cache line.
		const std::size_t offset_align =
			need < kCacheLineBytes ? need : kCacheLineBytes;

		std::size_t current = allocated_.load(std::memory_order_relaxed);
		std::size_t offset  = 0;
		std::size_t next    = 0;
		do {
			offset = round_up(current, offset_align);
			next   = offset + need;
			if (next > pool_size_) return nullptr; // out of arena memory
		} while (!allocated_.compare_exchange_weak(current,
												   next,
												   std::memory_order_acquire,
												   std::memory_order_relaxed));
		return memory_pool_ + offset;
	}

	/// @brief Return @p ptr to its size class's free list for reuse.
	/// @note @p bytes and @p align must match the allocate() call that produced
	///       @p ptr — they select the class the block goes back to.
	void deallocate(void *ptr, std::size_t bytes,
					std::align_val_t align) noexcept {
		if (ptr == nullptr || bytes > pool_size_) return;
		const std::size_t need =
			block_size(bytes, static_cast<std::size_t>(align));
		free_lists_[size_class(need)].push(ptr);
	}

private:
	static std::size_t round_up(std::size_t n, std::size_t multiple) noexcept {
		return (n + multiple - 1) & ~(multiple - 1);
	}

	/// @brief Power-of-two block actually handed out for a request. At least
	///        sizeof(void*) so a freed block can hold the free-list node, and at
	///        least @p align so the class's alignment guarantee covers it.
	static std::size_t block_size(std::size_t bytes,
								  std::size_t align) noexcept {
		std::size_t want = bytes < kMinBlock ? kMinBlock : bytes;
		if (want < align) want = align;
		return std::bit_ceil(want);
	}

	/// @brief Free-list index for a power-of-two block size.
	static std::size_t size_class(std::size_t block) noexcept {
		return static_cast<std::size_t>(std::countr_zero(block));
	}

	static constexpr std::align_val_t kPoolAlign{kCacheLineBytes};
	static constexpr std::size_t kMinBlock   = sizeof(void *);
	static constexpr std::size_t kSizeClasses = 64; ///< one per power of two

	std::uint8_t *memory_pool_{nullptr};
	std::size_t pool_size_{0};
	std::atomic<std::size_t> allocated_{0};
	FreeList free_lists_[kSizeClasses]{};
	bool numa_backed_{false};
};

} // namespace memory
