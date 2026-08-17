#pragma once

#include <boost/pool/pool.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace exchange::core::memory {

/**
 * @brief Single-threaded, fixed-capacity object pool over a Boost.Pool block.
 *
 * Takes @p size cells of raw storage as one contiguous block up front and never
 * asks the allocator for anything again: allocate() pops a cell and constructs
 * a @c T in it, free() destroys the @c T and pushes the cell back. LIFO on
 * purpose — the cell you just freed is the hottest in cache and the next one
 * handed out.
 *
 * @par Fixed size, and why there is no fallback
 * A drained pool returns @c nullptr. It does not grow, and it does not quietly
 * satisfy the request from the general allocator, because either would defeat
 * the reason the pool exists: an allocator call on the matching path is the
 * latency spike the pre-allocation was meant to remove, and cells minted
 * one-at-a-time land scattered across the heap, which is the fragmentation it
 * was meant to remove. A fallback also hides the mis-sizing — the pool keeps
 * working, just slower and less predictably, which is the worst way for a
 * trading system to fail. Size the pool to worst-case demand and treat a
 * @c nullptr as the capacity error it is.
 *
 * Boost.Pool would happily chain another block when the first runs out; the
 * live-cell count here is what holds it to the capacity it was built with, so
 * "one block, allocated once" is a guarantee rather than a hope.
 *
 * @par Why the free list is intrusive
 * A free cell holds no live object, so its bytes are dead space, and
 * @c boost::pool threads its @c next link through them. A parallel @c T** stack
 * would cost an extra @c size * sizeof(T*) bytes and touch a second cache line
 * on every allocate/free — a stack slot and the object it names are far apart,
 * so the two never share a line. Threading the link through the cell itself
 * makes the pop and the construction land on the *same* line.
 *
 * @par Address stability
 * The block is allocated once and never grown or moved, so a pointer handed out
 * by allocate() stays valid until it is freed — outstanding pointers survive any
 * number of later allocations. That is what lets callers link pooled objects to
 * one another by raw pointer, and fixed size is what makes it unconditional:
 * there is no reallocation that could ever move a live object.
 *
 * @note The pool does not track which cells are live, so it offers no iteration
 *       over its objects — a free cell's leading bytes hold a free-list link,
 *       not a @c T. A caller that needs to visit its live objects keeps them on
 *       its own intrusive list; the contiguous cells are then what make that
 *       walk cache-friendly.
 *
 * @warning NOT thread-safe. This is meant to be owned by a single thread (e.g.
 *          one pool per book side). Boost.Pool does no synchronisation and
 *          neither does this.
 *
 * @tparam T Payload type. Must be nothrow-destructible, since free() destroys
 *         it in place inside a @c noexcept function.
 */
template <typename T>
class object_pool {
	static_assert(std::is_nothrow_destructible_v<T>,
				  "free() destroys T in place inside a noexcept function");
	// Boost.Pool carves its block into equal chunks a multiple of a pointer
	// wide, from a base the system allocator aligned. Anything needing more
	// than pointer alignment would be handed a misaligned cell.
	static_assert(alignof(T) <= alignof(void *),
				  "boost::pool cannot honour an over-aligned payload");

	/// Cell width: at least a pointer, so the free list has somewhere to put
	/// its link while the cell holds no object. The padding is never
	/// observable, since it is read only while nothing lives here.
	static constexpr std::size_t CELL_BYTES =
		sizeof(T) > sizeof(void *) ? sizeof(T) : sizeof(void *);

	boost::pool<> storage_;      ///< one block, carved into cells
	std::size_t size_       = 0; ///< capacity
	std::size_t live_count_ = 0; ///< cells currently handed out

public:
	/**
	 * @brief Construct a pool of @p size cells, all initially free.
	 * @param size Pool capacity — the hard ceiling on simultaneously live
	 *        objects, since the pool never grows.
	 */
	explicit object_pool(std::uint32_t size)
		: storage_(CELL_BYTES, size > 0 ? size : 1), size_(size) {
		assert(size > 0 && "object_pool capacity must be non-zero");
		if (size_ == 0) return; // no cells: allocate() reports exhausted at once
		reserve_block();
	}

	/**
	 * @brief Release the block.
	 * @pre Every object still checked out has been freed. The pool does not
	 *      track which cells are live, so it cannot destroy them for you; a @c T
	 *      with a non-trivial destructor still outstanding here is a leaked
	 *      destructor, not a crash.
	 */
	~object_pool() = default;

	// Non-copyable, non-movable: outstanding pointers alias the storage.
	object_pool(const object_pool &)            = delete;
	object_pool &operator=(const object_pool &) = delete;
	object_pool(object_pool &&)                 = delete;
	object_pool &operator=(object_pool &&)      = delete;

	/// @brief Maximum number of objects the pool can hold. Fixed for its life.
	[[nodiscard]] std::uint32_t size() const noexcept {
		return static_cast<std::uint32_t>(size_);
	}

	/// @brief Number of cells currently available (not handed out).
	[[nodiscard]] std::size_t available() const noexcept {
		return size_ - live_count_;
	}

	/// @brief True when the next allocate() would return @c nullptr.
	[[nodiscard]] bool is_exhausted() const noexcept { return live_count_ == size_; }

	/**
	 * @brief Construct an object in a free cell and hand it out.
	 * @param args Constructor arguments forwarded to @c T; passing none
	 *        value-initialises it.
	 * @return The constructed object, or @c nullptr if the pool is drained —
	 *         a capacity error the caller must handle, not a slow path (see the
	 *         class docs).
	 * @note The returned object never carries a previous user's state. A free
	 *       cell's leading bytes hold the free list's link, so there is nothing
	 *       coherent left to retain — which is why this constructs rather than
	 *       handing back a recycled object.
	 */
	template <typename... Args>
	[[nodiscard]] T *allocate(Args &&...args) {
		if (is_exhausted()) [[unlikely]]
			return nullptr; // the pool never grows past its capacity
		void *block = storage_.malloc();
		if (block == nullptr) [[unlikely]]
			return nullptr;
		++live_count_;
		return std::construct_at(static_cast<T *>(block),
								 std::forward<Args>(args)...);
	}

	/**
	 * @brief Destroy an object and return its cell to the free list.
	 * @param obj Pointer previously returned by allocate() on *this* pool, not
	 *        already freed. @c nullptr is a no-op, so the result of a drained
	 *        allocate() can be handed back unchecked.
	 */
	void free(T *obj) noexcept {
		if (obj == nullptr) return;
		assert(owns(obj) && "free(): pointer is not a cell of this pool");
		assert(live_count_ > 0 && "free() underflow - double free?");
		std::destroy_at(obj);
		// free(), not ordered_free(): the ordered variant walks the free list to
		// keep it sorted by address, which is a scan on the hot path and buys
		// nothing here — a pool is a bag of interchangeable cells.
		storage_.free(obj);
		--live_count_;
	}

	/**
	 * @brief Make every cell free again.
	 * @warning Invalidates outstanding references and runs no destructors on
	 *          the objects still living in them — this is the bulk-discard
	 *          escape hatch for a trivially destructible @c T between runs, not
	 *          a substitute for free(). The block itself is replaced, so
	 *          addresses handed out before a reset do not come back after one.
	 */
	void reset() noexcept {
		storage_.purge_memory();
		live_count_ = 0;
		if (size_ == 0) return;
		// purge_memory leaves the next block sized by Boost's doubling schedule;
		// pin it back so a reset pool is the same shape as a fresh one.
		storage_.set_next_size(size_);
		reserve_block();
	}

private:
	/// @brief Does @p obj address a cell of this pool?
	///
	/// A debug guard, not a dispatch: with no heap fallback every pointer handed
	/// to free() must be one of ours, so a false here is a caller bug rather
	/// than a case to route around.
	[[nodiscard]] bool owns(const T *obj) const noexcept {
		// is_from takes a mutable void*; the check reads nothing through it.
		return storage_.is_from(const_cast<T *>(obj));
	}

	/// @brief Take the whole block now rather than at the first allocate.
	///
	/// Boost.Pool is lazy, and a pool whose storage appears on the first hot-path
	/// call is exactly the latency it exists to remove. One malloc/free forces
	/// the block out of the system allocator at construction, where the cost is
	/// affordable and the cells land as one dense run.
	void reserve_block() noexcept {
		void *first = storage_.malloc();
		assert(first != nullptr && "object_pool could not reserve its block");
		storage_.free(first);
	}
};
} // namespace exchange::core::memory
