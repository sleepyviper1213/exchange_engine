#pragma once

#include "memory/arena.hpp"

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

// Classic, stateful STL allocator over a pluggable "resource". A resource is any
// type modelling:
//     void *allocate(std::size_t bytes, std::size_t align);
//     void  deallocate(void *p, std::size_t bytes, std::size_t align) noexcept;
// MallocResource, ArenaResource, and Slab (see slab.hpp) all model it, so the
// same Allocator<T, R> drives std containers off the general heap, a NUMA/bump
// arena, or a fixed-block slab by swapping R.
namespace memory {

/**
 * @brief Resource routing to the global aligned allocator.
 *
 * Stateless. When the process is linked against tcmalloc or jemalloc, these
 * ::operator new/delete calls dispatch into that allocator transparently, so
 * selecting a faster malloc is a link-time choice, not a code change.
 */
class MallocResource {
public:
	[[nodiscard]] void *allocate(std::size_t bytes, std::size_t align) {
		return ::operator new(bytes, std::align_val_t{align});
	}
	void deallocate(void *p, std::size_t bytes, std::size_t align) noexcept {
		::operator delete(p, bytes, std::align_val_t{align});
	}
};

/// @brief The process-wide MallocResource instance (stateless, so shared).
[[nodiscard]] inline MallocResource &default_resource() noexcept {
	static MallocResource resource;
	return resource;
}

/**
 * @brief Adapts an Arena to the resource interface.
 *
 * Arena::deallocate takes only the pointer (blocks return to its free list), so
 * the size/align the allocator passes are ignored. Throws std::bad_alloc when
 * the arena is exhausted, matching allocate()'s contract.
 */
class ArenaResource {
public:
	explicit ArenaResource(Arena &arena) noexcept : arena_(&arena) {}

	[[nodiscard]] void *allocate(std::size_t bytes, std::size_t align) {
		void *p = arena_->allocate(bytes, std::align_val_t{align});
		if (p == nullptr) throw std::bad_alloc();
		return p;
	}
	void deallocate(void *p, std::size_t /*bytes*/,
					std::size_t /*align*/) noexcept {
		arena_->deallocate(p);
	}

private:
	Arena *arena_;
};

/**
 * @brief Stateful STL allocator drawing from a @tparam Resource.
 *
 * Holds a pointer to the resource, so a container carries its storage source
 * with it. Two allocators compare equal iff they share a resource — the signal
 * std containers use to decide whether one's storage can be adopted by another
 * on move/swap. A default-constructed allocator uses default_resource(); that
 * ctor exists only when Resource is the stateless MallocResource, since every
 * other resource must be supplied explicitly.
 *
 * @note A single-size-class resource (e.g. Slab) suits node-based containers
 *       (std::list, std::unordered_map) where every allocation is one node;
 *       contiguous containers that grow past the block size need a general
 *       resource such as MallocResource or ArenaResource.
 */
template <class T, class Resource = MallocResource>
class Allocator {
public:
	using value_type                             = T;
	using size_type                              = std::size_t;
	using difference_type                        = std::ptrdiff_t;
	using propagate_on_container_move_assignment = std::true_type;
	using propagate_on_container_swap            = std::true_type;

	template <class U>
	struct rebind {
		using other = Allocator<U, Resource>;
	};

	/// @brief Default to the shared MallocResource (MallocResource only).
	Allocator() noexcept
		requires std::is_same_v<Resource, MallocResource>
		: resource_(&default_resource()) {}

	/// @brief Draw allocations from @p resource.
	explicit Allocator(Resource &resource) noexcept : resource_(&resource) {}

	/// @brief Rebinding conversion: shares the source allocator's resource.
	template <class U>
	Allocator(const Allocator<U, Resource> &other) noexcept
		: resource_(other.resource()) {}

	[[nodiscard]] T *allocate(std::size_t n) {
		if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
			throw std::bad_array_new_length();
		return static_cast<T *>(resource_->allocate(n * sizeof(T), alignof(T)));
	}

	void deallocate(T *p, std::size_t n) noexcept {
		resource_->deallocate(p, n * sizeof(T), alignof(T));
	}

	[[nodiscard]] Resource *resource() const noexcept { return resource_; }

private:
	Resource *resource_;
};

template <class T, class U, class R>
[[nodiscard]] bool operator==(const Allocator<T, R> &a,
							  const Allocator<U, R> &b) noexcept {
	return a.resource() == b.resource();
}
template <class T, class U, class R>
[[nodiscard]] bool operator!=(const Allocator<T, R> &a,
							  const Allocator<U, R> &b) noexcept {
	return !(a == b);
}

} // namespace memory
