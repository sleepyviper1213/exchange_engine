#ifdef ORDER_BOOK_WITH_NUMA
#include "numa_arena_allocator.hpp"

#include <numa.h>  // numa_node_of_cpu
#include <sched.h> // sched_getcpu

namespace exchange::core::memory {

numa_arena_allocator::numa_arena_allocator(std::size_t arena_size) {
	for (int node = 0; node < static_cast<int>(kMaxNodes); ++node)
		arenas_[node].init(arena_size, node);
}

void *numa_arena_allocator::alloc(std::size_t bytes,
								  std::align_val_t align) noexcept {
	arena &node_arena = arenas_[current_node()];

	// Retry a few times to ride out lock-free CAS contention.
	for (int attempt = 0; attempt < 3; ++attempt)
		if (void *ptr = node_arena.allocate(bytes, align)) return ptr;

	// Arena exhausted: fall back to the system allocator.
	return ::operator new(bytes, align, std::nothrow);
}

void numa_arena_allocator::dealloc(void *ptr, std::size_t bytes,
								   std::align_val_t align) noexcept {
	if (ptr == nullptr) return;
	arenas_[current_node()].deallocate(ptr, bytes, align);
}

std::size_t numa_arena_allocator::current_node() const noexcept {
	const int cpu         = ::sched_getcpu();
	const int node        = cpu < 0 ? 0 : ::numa_node_of_cpu(cpu);
	const std::size_t idx = node < 0 ? 0 : static_cast<std::size_t>(node);
	return idx < kMaxNodes ? idx : kMaxNodes - 1;
}
} // namespace exchange::core::memory

#endif
