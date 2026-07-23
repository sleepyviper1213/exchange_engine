#pragma once

#include "memory/free_list.hpp"

#include <atomic>
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
 * allocate() serves the free list first (O(1) reuse of returned blocks), then
 * bumps a monotonically increasing offset. The arena never returns memory to the
 * OS during its life, so handed-out addresses are stable — which is also what
 * makes the intrusive FreeList's ABA assumption hold.
 */
struct alignas(kCacheLineBytes) Arena {
	Arena() noexcept                = default;
	Arena(const Arena &)            = delete;
	Arena &operator=(const Arena &) = delete;

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

	~Arena() {
		if (memory_pool_ == nullptr) return;
#if defined(__linux__)
		if (numa_backed_) {
			::numa_free(memory_pool_, pool_size_);
			return;
		}
#endif
		::operator delete(memory_pool_, kPoolAlign);
	}

	/// @brief Try to satisfy a @p bytes / @p align request: free list first,
	///        then bump the pointer.
	/// @return Pointer to the block, or nullptr if this arena cannot serve it.
	[[nodiscard]] void *allocate(std::size_t bytes,
								 std::align_val_t align) noexcept {
		// O(1) fast path: reuse a returned block.
		if (void *reused = free_list_.pop()) return reused;

		// Bump path. Align the *offset* (not just the size) so every returned
		// block satisfies its alignment; the pool base is over-aligned to
		// kPoolAlign, so an aligned offset yields an aligned pointer.
		const auto alignment   = static_cast<std::size_t>(align);
		const std::size_t need = round_up(bytes, alignment);

		std::size_t current = allocated_.load(std::memory_order_relaxed);
		std::size_t offset  = 0;
		std::size_t next    = 0;
		do {
			offset = round_up(current, alignment);
			next   = offset + need;
			if (next > pool_size_) return nullptr; // out of arena memory
		} while (!allocated_.compare_exchange_weak(current,
												   next,
												   std::memory_order_acquire,
												   std::memory_order_relaxed));
		return memory_pool_ + offset;
	}

	/// @brief Return @p ptr to the free list for reuse.
	void deallocate(void *ptr) noexcept { free_list_.push(ptr); }

private:
	static std::size_t round_up(std::size_t n, std::size_t multiple) noexcept {
		return (n + multiple - 1) & ~(multiple - 1);
	}

	static constexpr std::align_val_t kPoolAlign{kCacheLineBytes};

	std::uint8_t *memory_pool_{nullptr};
	std::size_t pool_size_{0};
	std::atomic<std::size_t> allocated_{0};
	FreeList free_list_{};
	bool numa_backed_{false};
};

} // namespace memory
