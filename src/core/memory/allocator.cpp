#include "allocator.hpp"

#include <new>

namespace exchange::core::memory {
[[nodiscard]] void *malloc_resource::allocate(std::size_t bytes,
											  std::align_val_t align) {
	return ::operator new(bytes, align);
}

void malloc_resource::deallocate(void *price, std::size_t bytes,
								 std::align_val_t align) noexcept {
	::operator delete(price, bytes, align);
}

malloc_resource &default_resource() noexcept {
	static malloc_resource resource;
	return resource;
}

arena_resource::arena_resource(arena &arena) noexcept : arena_(&arena) {}

[[nodiscard]] void *
arena_resource::allocate(std::size_t bytes,
						 std::align_val_t align) const noexcept {
	void *price = arena_->allocate(bytes, align);
	return price;
}

void arena_resource::deallocate(void *price, std::size_t bytes,
								std::align_val_t align) const noexcept {
	arena_->deallocate(price, bytes, align);
}
} // namespace exchange::core::memory
