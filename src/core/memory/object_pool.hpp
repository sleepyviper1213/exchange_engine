#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exchange::core::memory {

/**
 * @brief Single-threaded, fixed-capacity object pool (LIFO free-list).
 *
 * Owns @p size objects up front and tracks which are free with a stack of
 * pointers: allocate() pops the top, free() pushes it back. LIFO on purpose —
 * the object you just freed is the hottest in cache and the next one handed out.
 *
 * A pool is a *bag* of interchangeable objects, so there is no ordering to
 * maintain and no need for the sequence numbers / dual cursors a queue carries.
 * This keeps allocate()/free() to a couple of loads and a store each.
 *
 * @warning NOT thread-safe. This is meant to be owned by a single thread (e.g.
 *          one pool per book side). If a pool is genuinely shared across
 *          threads, use a lock-free Treiber stack instead — this type does no
 *          synchronisation at all.
 *
 * @tparam T Payload type; must be default constructible (objects are created up
 *         front and by the heap fallback).
 */
template <typename T>
class object_pool {
	static_assert(std::is_default_constructible_v<T>,
				  "T must be default constructible");

	T *storage_          = nullptr; ///< the pooled objects (constructed once)
	T **free_            = nullptr; ///< LIFO stack of currently-free objects
	std::size_t size_    = 0;       ///< capacity
	std::size_t free_top_ = 0;      ///< count of free objects (stack height)
	std::intptr_t lower_bound_ = 0; ///< low address bound for ownership check
	std::intptr_t upper_bound_ = 0; ///< high address bound for ownership check

public:
	/**
	 * @brief Construct a pool of @p size objects, all initially free.
	 * @param size Pool capacity.
	 */
	explicit object_pool(std::uint32_t size)
		: storage_(new T[size]),
		  free_(new T *[size]),
		  size_(size),
		  free_top_(size) {
		assert(size > 0 && "object_pool capacity must be non-zero");
		for (std::size_t i = 0; i < size_; ++i) free_[i] = &storage_[i];
		lower_bound_ = reinterpret_cast<std::intptr_t>(&storage_[0]);
		upper_bound_ = reinterpret_cast<std::intptr_t>(&storage_[size_ - 1]);
	}

	~object_pool() {
		delete[] storage_;
		delete[] free_;
	}

	// Non-copyable, non-movable: outstanding pointers alias the storage.
	object_pool(const object_pool &)            = delete;
	object_pool &operator=(const object_pool &) = delete;
	object_pool(object_pool &&)                 = delete;
	object_pool &operator=(object_pool &&)      = delete;

	/// @brief Maximum number of objects the pool can hold.
	[[nodiscard]] std::uint32_t size() const noexcept {
		return static_cast<std::uint32_t>(size_);
	}

	/// @brief Number of objects currently available (not handed out).
	[[nodiscard]] std::size_t available() const noexcept { return free_top_; }

	/**
	 * @brief Allocate an object.
	 * @return A pooled object, or a heap-allocated one if the pool is drained.
	 *         Never nullptr. Heap-allocated objects are reclaimed by free().
	 * @note The returned object retains whatever state a previous user left in
	 *       it (the pool does not re-initialize) — construct/assign before use,
	 *       as with any pool.
	 */
	T *allocate() {
		if (free_top_ > 0) return free_[--free_top_];
		return new T(); // pool exhausted - heap fallback
	}

	/**
	 * @brief Return an object.
	 * @details Pool-owned pointers (recognised by address range) go back on the
	 *          free stack; heap-fallback pointers are deleted.
	 * @param obj Pointer previously returned by allocate(); not null, not freed
	 *            twice.
	 */
	void free(T *obj) {
		const auto o = reinterpret_cast<std::intptr_t>(obj);
		if (o >= lower_bound_ && o <= upper_bound_) {
			assert(free_top_ < size_ && "free() overflow — double free?");
			free_[free_top_++] = obj;
		} else {
			delete obj; // heap-allocated (pool was exhausted) - reclaim it
		}
	}

	/// @brief Make every object free again. Invalidates outstanding references.
	void reset() noexcept {
		for (std::size_t i = 0; i < size_; ++i) free_[i] = &storage_[i];
		free_top_ = size_;
	}
};
} // namespace exchange::core::memory
