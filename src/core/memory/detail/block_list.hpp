#pragma once
// The free list behind one of `arena`'s size classes.
//
// It was a nested class in the arena's private section, so every reader of
// arena.hpp met the recycling mechanism before the allocator interface. Nothing
// outside the arena has ever named it, and now the header says so.

#include "../fwd.hpp"

#include <cstddef>
#include <cstring>

namespace exchange::core::memory::detail {

/**
 * @brief A LIFO stack of same-sized free blocks, linked through the blocks.
 *
 * Intrusive and pointer-sized: a returned block holds no live object, so its
 * leading bytes are dead space and the link goes there. That is why a size
 * class never rounds below @c MIN_BLOCK_BYTES — a block too small to hold the
 * link could not be recycled at all.
 *
 * Deliberately plain: the arena is thread-confined (see its class note), so the
 * lists need no atomics, and a size class is a bag of interchangeable blocks,
 * so there is no ordering to maintain. LIFO also hands back the block most
 * recently touched, which is the one still in cache.
 */
class block_list {
public:
	/// @brief Smallest block that can host the link, and therefore the smallest
	///        size class the arena will hand out.
	static constexpr std::size_t MIN_BLOCK_BYTES = sizeof(void *);

	/// @brief Return @p block to this class for reuse.
	void push(void *block) noexcept {
		// memcpy rather than a cast-and-store: the block holds no object, so
		// there is no pointer there to alias, only bytes to write.
		std::memcpy(block, &head_, sizeof head_);
		head_ = block;
	}

	/// @brief Take a block back, or @c nullptr when the class is empty.
	[[nodiscard]] void *pop() noexcept {
		if (head_ == nullptr) return nullptr;
		void *block = head_;
		std::memcpy(&head_, block, sizeof head_);
		return block;
	}

private:
	void *head_ = nullptr;
};

} // namespace exchange::core::memory::detail
