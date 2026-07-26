#include "slab.hpp"

#include "core/util/round_up.hpp"

#include <new>

namespace exchange::core::memory {

slab::slab(std::size_t block_size, std::align_val_t block_align,
		   std::size_t blocks_per_slab)
	: block_align_(block_align),
	  blocks_per_slab_(std::max(blocks_per_slab, 1uz)) {
	stride_ =
		util::round_up(std::max(block_size, sizeof(void *)), block_align_);
}

slab::~slab() {
	for (void *slab : slabs_)
		::operator delete(slab, slab_bytes(), std::align_val_t{block_align_});
}

[[nodiscard]] void *slab::allocate() {
	if (free_head_ == nullptr) grow();
	void *block = free_head_;
	free_head_  = *reinterpret_cast<void **>(free_head_); // pop
	++outstanding_;
	return block;
}

void slab::deallocate(void *block) noexcept {
	if (block == nullptr) return;
	*reinterpret_cast<void **>(block) = free_head_; // push
	free_head_                        = block;
	--outstanding_;
}

[[nodiscard]] void *slab::allocate(std::size_t bytes, std::align_val_t align) {
	assert(bytes <= stride_ && align <= block_align_ &&
		   "request exceeds this Slab's block size/alignment");
	(void)bytes;
	(void)align;
	return allocate();
}

void slab::deallocate(void *block, std::size_t /*bytes*/,
					  std::align_val_t /*align*/) noexcept {
	deallocate(block);
}

[[nodiscard]] std::size_t slab::block_size() const noexcept { return stride_; }

[[nodiscard]] std::align_val_t slab::block_align() const noexcept {
	return block_align_;
}

[[nodiscard]] std::size_t slab::outstanding() const noexcept {
	return outstanding_;
}

[[nodiscard]] std::size_t slab::slab_bytes() const noexcept {
	return stride_ * blocks_per_slab_;
}

void slab::grow() {
	void *slab = ::operator new(slab_bytes(), std::align_val_t{block_align_});
	slabs_.push_back(slab);
	auto *base = static_cast<std::byte *>(slab);
	for (std::size_t i = 0; i < blocks_per_slab_; ++i) {
		void *block                       = base + i * stride_;
		*reinterpret_cast<void **>(block) = free_head_;
		free_head_                        = block;
	}
}
} // namespace memory
