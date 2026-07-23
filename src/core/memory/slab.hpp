#pragma once

#include <cassert>
#include <cstddef>
#include <new>
#include <vector>

namespace memory {

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
class Slab {
public:
	/// @brief Construct a slab allocator.
	/// @param block_size Bytes per block. Rounded up to hold at least a free-list
	///        link and to a multiple of @p block_align.
	/// @param block_align Alignment every block is guaranteed. Must be a power of
	///        two. Defaults to the platform max useful alignment.
	/// @param blocks_per_slab Blocks carved per system allocation. Larger means
	///        fewer, bigger system allocations.
	explicit Slab(std::size_t block_size,
				  std::size_t block_align = alignof(std::max_align_t),
				  std::size_t blocks_per_slab = 1024)
		: block_align_(block_align),
		  blocks_per_slab_(blocks_per_slab == 0 ? 1 : blocks_per_slab) {
		assert(block_align_ != 0 && (block_align_ & (block_align_ - 1)) == 0 &&
			   "block_align must be a power of two");
		// A block must hold at least the free-list link, and its stride must keep
		// every block aligned, so round up to a multiple of the alignment.
		std::size_t stride = block_size < sizeof(void *) ? sizeof(void *) : block_size;
		stride    = round_up(stride, block_align_);
		stride_   = stride;
	}

	Slab(const Slab &)            = delete;
	Slab &operator=(const Slab &) = delete;

	~Slab() {
		for (void *slab : slabs_)
			::operator delete(slab, slab_bytes(), std::align_val_t{block_align_});
	}

	/// @brief Allocate one block (>= the configured block_size, aligned to
	///        block_align). Never returns nullptr — grows on exhaustion.
	[[nodiscard]] void *allocate() {
		if (free_head_ == nullptr) grow();
		void *block = free_head_;
		free_head_  = *reinterpret_cast<void **>(free_head_); // pop
		++outstanding_;
		return block;
	}

	/// @brief Return a block previously handed out by this Slab.
	void deallocate(void *block) noexcept {
		if (block == nullptr) return;
		*reinterpret_cast<void **>(block) = free_head_; // push
		free_head_                        = block;
		--outstanding_;
	}

	/// @brief Resource-style allocate for Allocator<T, Slab>. The request must
	///        fit a block; a Slab is a single-size-class allocator by design.
	[[nodiscard]] void *allocate(std::size_t bytes, std::size_t align) {
		assert(bytes <= stride_ && align <= block_align_ &&
			   "request exceeds this Slab's block size/alignment");
		(void)bytes;
		(void)align;
		return allocate();
	}
	void deallocate(void *block, std::size_t /*bytes*/,
					std::size_t /*align*/) noexcept {
		deallocate(block);
	}

	/// @brief Bytes guaranteed usable per block (>= the requested block_size).
	[[nodiscard]] std::size_t block_size() const noexcept { return stride_; }
	/// @brief Alignment guaranteed for every block.
	[[nodiscard]] std::size_t block_align() const noexcept { return block_align_; }
	/// @brief Blocks currently handed out and not yet returned.
	[[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_; }

private:
	static std::size_t round_up(std::size_t n, std::size_t multiple) noexcept {
		return (n + multiple - 1) & ~(multiple - 1);
	}
	std::size_t slab_bytes() const noexcept { return stride_ * blocks_per_slab_; }

	// Allocate one slab and thread all of its blocks onto the free list.
	void grow() {
		void *slab = ::operator new(slab_bytes(), std::align_val_t{block_align_});
		slabs_.push_back(slab);
		auto *base = static_cast<std::byte *>(slab);
		for (std::size_t i = 0; i < blocks_per_slab_; ++i) {
			void *block                       = base + i * stride_;
			*reinterpret_cast<void **>(block) = free_head_;
			free_head_                        = block;
		}
	}

	std::size_t block_align_;
	std::size_t blocks_per_slab_;
	std::size_t stride_       = 0;       ///< actual bytes between block starts
	void *free_head_          = nullptr; ///< intrusive free list head
	std::size_t outstanding_  = 0;       ///< blocks currently in use
	std::vector<void *> slabs_;          ///< owned system allocations
};

} // namespace memory
