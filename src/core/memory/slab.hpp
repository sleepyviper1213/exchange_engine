#pragma once

#include "fwd.hpp"

#include <cassert>
#include <cstddef>
#include <new>
#include <vector>

namespace exchange::core::memory {

/**
 * @brief Growable, single-threaded allocator for fixed-size, fixed-alignment
 *        blocks.
 *
 * Carves large "slabs" from the system allocator and hands out equally sized
 * blocks cut from them. Freed blocks go on an intrusive free list (the "next"
 * pointer lives in the block's own storage), so allocate()/deallocate() are a
 * pop/push each. When every block is in use, one more slab is allocated and its
 * blocks are threaded onto the free list — the arena grows, and never shrinks
 * until destruction, so block addresses are stable for their lifetime.
 *
 * Ideal for uniform node allocation (one Slab per node type) where the general
 * heap's per-object bookkeeping and size-class searching are pure overhead.
 *
 * @warning NOT thread-safe. One Slab per owning thread.
 */
class slab {
public:
	/// @brief Construct a slab allocator.
	/// @param block_size Bytes per block. Rounded up to hold at least a
	/// free-list
	///        link and to a multiple of @p block_align.
	/// @param block_align Alignment every block is guaranteed. Must be a power
	/// of
	///        two. Defaults to the platform max useful alignment.
	/// @param blocks_per_slab Blocks carved per system allocation. Larger means
	///        fewer, bigger system allocations.

	CORE_AUTOTEST_EXPORT explicit slab(
		std::size_t block_size,
		std::align_val_t block_align = std::align_val_t{alignof(std::max_align_t)},
		std::size_t blocks_per_slab = 1024);

	slab(const slab &)            = delete;
	slab &operator=(const slab &) = delete;

	CORE_AUTOTEST_EXPORT ~slab();

	/// @brief Allocate one block (>= the configured block_size, aligned to
	///        block_align). Never returns nullptr — grows on exhaustion.
	[[nodiscard]] CORE_AUTOTEST_EXPORT void *allocate();

	/// @brief Return a block previously handed out by this Slab.
	CORE_AUTOTEST_EXPORT void deallocate(void *block) noexcept;

	/// @brief Resource-style allocate for allocator<T, Slab>. The request must
	///        fit a block; a Slab is a single-size-class allocator by design.
	[[nodiscard]] CORE_AUTOTEST_EXPORT void *allocate(std::size_t bytes,
													  std::align_val_t align);

	CORE_AUTOTEST_EXPORT void deallocate(void *block, std::size_t /*bytes*/,
										 std::align_val_t /*align*/) noexcept;

	/// @brief Bytes guaranteed usable per block (>= the requested block_size).
	[[nodiscard]] CORE_AUTOTEST_EXPORT std::size_t block_size() const noexcept;

	/// @brief Alignment guaranteed for every block.
	[[nodiscard]] CORE_AUTOTEST_EXPORT std::align_val_t
	block_align() const noexcept;

	/// @brief Blocks currently handed out and not yet returned.
	[[nodiscard]] CORE_AUTOTEST_EXPORT std::size_t outstanding() const noexcept;

private:
	[[nodiscard]] std::size_t slab_bytes() const noexcept;

	// Allocate one slab and thread all of its blocks onto the free list.
	void grow();

	std::align_val_t block_align_;
	std::size_t blocks_per_slab_;
	// A block must hold at least the free-list link, and its stride must
	// keep every block aligned, so round up to a multiple of the alignment.
	std::size_t stride_      = 0;       ///< actual bytes between block starts
	void *free_head_         = nullptr; ///< intrusive free list head
	std::size_t outstanding_ = 0;       ///< blocks currently in use
	std::vector<void *> slabs_;         ///< owned system allocations
};

} // namespace memory
