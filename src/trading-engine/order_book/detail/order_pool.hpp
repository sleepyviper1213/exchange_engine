#pragma once

#include "resting_order.hpp"

#include <boost/pool/pool.hpp>

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace exchange::engine::detail {

/**
 * @brief Fixed-size-cell pool for the nodes an intrusive book links together.
 *
 * One contiguous block of @p capacity cells is taken at the first @c acquire
 * and never given back until the pool dies, so a book that stays inside its
 * hint never calls the general allocator again: @c acquire pops the free list
 * and constructs in place, @c release destroys and pushes the cell back. Both
 * are a couple of loads and a store — @c boost::pool's unordered free list is
 * LIFO, so the cell handed out next is the one just released and therefore the
 * hottest in cache.
 *
 * @par Why this and not @c boost::object_pool
 * @c object_pool::destroy goes through @c ordered_free, which walks the free
 * list to keep it sorted by address — O(free cells) on the cancel path, which
 * is exactly where an exchange cannot afford a walk. This wraps the plain
 * @c boost::pool and pays for the ordering nowhere.
 *
 * @par Address stability
 * Cells never move. Growing means chaining another block, not reallocating the
 * old one, so a pointer handed out by @c acquire stays valid until it is
 * released no matter how many orders arrive afterwards. That is the property
 * intrusive linking rests on: a level's FIFO is a chain of raw pointers between
 * cells, and one moved cell would break every link into it.
 *
 * @warning Not thread safe, by design — a matching core is one thread on one
 *          core, and a lock in here would be a lock in the matching loop.
 *
 * @tparam T Node type. Must be nothrow-destructible, since @c release destroys
 *         it in place inside a @c noexcept function.
 */
template <typename T>
class basic_pool {
	static_assert(std::is_nothrow_destructible_v<T>,
				  "release() destroys T in place inside a noexcept function");
	// boost::pool hands out cells that are a multiple of a pointer apart,
	// starting from a block that new[] aligned. Anything needing more than
	// pointer alignment would be handed a misaligned cell.
	static_assert(alignof(T) <= alignof(void *),
				  "boost::pool cannot honour over-aligned node types");

public:
	/// @brief Cells taken in the first block when no hint is given.
	static constexpr std::size_t DEFAULT_CAPACITY = 1U << 10;

	/**
	 * @brief A pool whose first block holds @p capacity cells.
	 * @param capacity Expected number of simultaneously live nodes. Exceeding
	 *        it is correct but chains a second block, so the nodes stop being
	 *        one dense run; size it to worst-case book depth.
	 */
	explicit basic_pool(std::size_t capacity = DEFAULT_CAPACITY)
		: storage_(sizeof(T), capacity > 0 ? capacity : DEFAULT_CAPACITY) {}

	// Non-copyable, non-movable: live nodes point into the blocks.
	basic_pool(const basic_pool &)            = delete;
	basic_pool &operator=(const basic_pool &) = delete;
	basic_pool(basic_pool &&)                 = delete;
	basic_pool &operator=(basic_pool &&)      = delete;

	/**
	 * @brief Take a cell and construct a @c T in it from @p args.
	 * @return The constructed node, or @c nullptr if the pool could not obtain
	 *         another block — the capacity error it is, reported rather than
	 *         thrown. An exception unwinding out of the matching loop would
	 *         leave a half-matched order behind, and there is nothing to catch
	 *         it on a path that has already printed trades.
	 * @warning Every caller must handle the null. It reaches the client as a
	 *          CANCELLED / BOOK_AT_CAPACITY outcome, not as a dropped order.
	 */
	template <typename... Args>
	[[nodiscard]] T *acquire(Args &&...args) noexcept {
		void *cell = storage_.malloc();
		if (cell == nullptr) [[unlikely]]
			return nullptr;
		return std::construct_at(static_cast<T *>(cell),
								 std::forward<Args>(args)...);
	}

	/// @brief Destroy @p node and return its cell. @c nullptr is a no-op.
	/// @pre @p node came from this pool and has been unlinked from whatever
	///      intrusive container held it.
	void release(T *node) noexcept {
		if (node == nullptr) return;
		assert(storage_.is_from(node) &&
			   "release(): node is not from this pool");
		std::destroy_at(node);
		storage_.free(node); // unordered: pushes the cell, no walk
	}

private:
	boost::pool<> storage_;
};

/// @brief The pool every resting order in a book is drawn from.
///
/// One pool per book, not one per level: a level is a transient thing that
/// appears when someone quotes a price and vanishes when the last order there
/// leaves, and giving each its own cells would scatter the nodes the matching
/// loop walks across as many blocks as there are prices.
using order_pool = basic_pool<resting_order>;

} // namespace exchange::engine::detail
