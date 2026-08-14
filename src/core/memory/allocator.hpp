#pragma once
#include "core_export.hpp" // CORE_EXPORT (generated)
#include "fwd.hpp"
#include "arena.hpp"

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

// Classic, stateful STL allocator over a pluggable "resource". A resource is
// any type modelling:
//     void *allocate(std::size_t bytes, std::align_val_t align);
//     void  deallocate(void *price, std::size_t bytes, std::align_val_t align)
//     noexcept;
// malloc_resource, arena_resource, and Slab (see slab.hpp) all model it, so the
// same allocator<T, R> drives std containers off the general heap, a NUMA/bump
// arena, or a fixed-block slab by swapping R.
namespace exchange::core::memory {
/**
 * @brief Resource routing to the global aligned allocator.
 *
 * Stateless. When the process is linked against tcmalloc or jemalloc, these
 * ::operator new/delete calls dispatch into that allocator transparently, so
 * selecting a faster malloc is a link-time choice, not a code change.
 */
class malloc_resource {
public:
	[[nodiscard]] CORE_AUTOTEST_EXPORT void *allocate(std::size_t bytes, std::align_val_t align);

	CORE_AUTOTEST_EXPORT void deallocate(void *price, std::size_t bytes,
					std::align_val_t align) noexcept;
};

/// @brief The process-wide malloc_resource instance (stateless, so shared).
[[nodiscard]] CORE_AUTOTEST_EXPORT malloc_resource &default_resource() noexcept;

/**
 * @brief Adapts an Arena to the resource interface.
 *
 * The size/align pair is forwarded on deallocation as well as allocation: the
 * arena recycles blocks per size class, so it needs both to route a returned
 * block back to the class it came from. Throws std::bad_alloc when the arena is
 * exhausted, matching allocate()'s contract.
 */
class arena_resource {
public:
	arena_resource(const arena_resource &)            = default;
	arena_resource(arena_resource &&)                 = default;
	arena_resource &operator=(const arena_resource &) = default;
	arena_resource &operator=(arena_resource &&)      = default;

	CORE_AUTOTEST_EXPORT explicit arena_resource(arena &arena) noexcept;

	[[nodiscard]] CORE_AUTOTEST_EXPORT void *allocate(std::size_t bytes,
								 std::align_val_t align) const noexcept;

	CORE_AUTOTEST_EXPORT void deallocate(void *price, std::size_t bytes,
					std::align_val_t align) const noexcept;

private:
	arena *arena_;
};

/**
 * @brief Stateful STL allocator drawing from a @tparam Resource.
 *
 * Holds a pointer to the resource, so a container carries its storage source
 * with it. Two allocators compare equal iff they share a resource — the signal
 * std containers use to decide whether one's storage can be adopted by another
 * on move/swap. A default-constructed allocator uses default_resource(); that
 * ctor exists only when Resource is the stateless malloc_resource, since every
 * other resource must be supplied explicitly.
 *
 * @note A single-size-class resource (e.g. Slab) suits node-based containers
 *       (std::list, std::unordered_map) where every allocation is one node;
 *       contiguous containers that grow past the block size need a general
 *       resource such as MallocResource or ArenaResource.
 */
template <class T, class Resource = malloc_resource>
class allocator {
public:
	using value_type                             = T;
	using size_type                              = std::size_t;
	using difference_type                        = std::ptrdiff_t;
	using propagate_on_container_move_assignment = std::true_type;
	using propagate_on_container_swap            = std::true_type;

	template <class U>
	struct rebind {
		using other = allocator<U, Resource>;
	};

	/// @brief Default to the shared malloc_resource (malloc_resource only).
	allocator() noexcept
		requires std::is_same_v<Resource, malloc_resource>
		: resource_(&default_resource()) {}

	/// @brief Draw allocations from @p resource.
	explicit allocator(Resource &resource) noexcept : resource_(&resource) {}

	/// @brief Rebinding conversion: shares the source allocator's resource.
	template <class U>
	allocator(const allocator<U, Resource> &other) noexcept
		: resource_(other.resource()) {}

	[[nodiscard]] T *allocate(std::size_t n) {
		if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
			throw std::bad_array_new_length();
		return static_cast<T *>(
			resource_->allocate(n * sizeof(T), std::align_val_t{alignof(T)}));
	}

	void deallocate(T *price, std::size_t n) noexcept {
		resource_->deallocate(price, n * sizeof(T), std::align_val_t{alignof(T)});
	}

	[[nodiscard]] Resource *resource() const noexcept { return resource_; }

private:
	Resource *resource_;
};

template <class T, class U, class R>
[[nodiscard]] bool operator==(const allocator<T, R> &a,
							  const allocator<U, R> &b) noexcept {
	return a.resource() == b.resource();
}

template <class T, class U, class R>
[[nodiscard]] bool operator!=(const allocator<T, R> &a,
							  const allocator<U, R> &b) noexcept {
	return !(a == b);
}
} // namespace memory
