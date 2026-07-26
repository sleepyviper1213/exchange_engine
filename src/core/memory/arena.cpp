#include "arena.hpp"

#include "core/util/round_up.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <new>

namespace exchange::core::memory {

void arena::init(std::size_t size) {
	pool_size_   = size;
	numa_backed_ = false;
	memory_pool_ = static_cast<std::uint8_t *>(
		::operator new(size, kPoolAlign, std::nothrow));
	if (memory_pool_ == nullptr) std::abort();
}

#ifdef __linux__
void arena::init(std::size_t size, int node) {
	if (::numa_available() < 0) {
		init(size);
		return;
	}
	pool_size_   = size;
	numa_backed_ = true;
	memory_pool_ = static_cast<std::uint8_t *>(::numa_alloc_onnode(size, node));
	if (memory_pool_ == nullptr) std::abort();
}
#endif

arena::~arena() {
	if (memory_pool_ == nullptr) return;
#if defined(__linux__)
	if (numa_backed_) {
		::numa_free(memory_pool_, pool_size_);
		return;
	}
#endif
	::operator delete(memory_pool_, kPoolAlign);
}

[[nodiscard]] void *arena::allocate(std::size_t bytes,
									std::align_val_t align) noexcept {
	if (bytes > pool_size_) return nullptr; // never satisfiable here
	const std::size_t need = block_size(bytes, align);

	// O(1) fast path: reuse a block returned by this same size class. Any
	// block in the class is `need` bytes and at least as aligned as any
	// request the class accepts, so the reuse is a genuine fit.
	if (void *reused = free_lists_[size_class(need)].pop()) return reused;

	// Bump path. Align the *offset* (not just the size) so every returned
	// block satisfies its alignment; the pool base is over-aligned to
	// kPoolAlign, so an aligned offset yields an aligned pointer. Blocks
	// below a cache line align to their own (power-of-two) size, which is
	// >= the requested alignment; larger ones align to the cache line.
	const std::align_val_t offset_align{
		std::max(need, std::hardware_destructive_interference_size)};

	std::size_t current = allocated_.load(std::memory_order_relaxed);
	std::size_t offset  = 0;
	std::size_t next    = 0;
	do {
		offset = util::round_up(current, offset_align);
		next   = offset + need;
		if (next > pool_size_) return nullptr; // out of arena memory
	} while (!allocated_.compare_exchange_weak(current,
											   next,
											   std::memory_order_relaxed,
											   std::memory_order_relaxed));
	return memory_pool_ + offset;
}

void arena::deallocate(void *ptr, std::size_t bytes,
					   std::align_val_t align) noexcept {
	if (ptr == nullptr || bytes > pool_size_) return;
	const std::size_t need = block_size(bytes, align);
	free_lists_[size_class(need)].push(ptr);
}

std::size_t arena::block_size(std::size_t bytes,
							  std::align_val_t align) noexcept {
	const auto want = std::max({bytes, kMinBlock, static_cast<size_t>(align)});

	return std::bit_ceil(want);
}

std::size_t arena::size_class(std::size_t block) noexcept {
	return static_cast<std::size_t>(std::countr_zero(block));
}
} // namespace memory
