#pragma once

#include "pool_growth.hpp" // IWYU pragma: export
#include "resting_order.hpp"

#include <boost/pool/pool.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace exchange::engine::detail {

/**
 * @brief Fixed-size-cell pool for the nodes an intrusive book links together.
 *
 * One contiguous block of @p capacity cells is taken **at construction** and
 * never given back until the pool dies, so a book that stays inside its hint
 * never calls the general allocator again: @c acquire pops the free list and
 * constructs in place, @c release destroys and pushes the cell back. Both are a
 * couple of loads and a store.
 *
 * @par Why construction and not first use
 * @c boost::pool allocates nothing in its own constructor - it records the
 * block size and waits. Left alone, the first order a book ever rests therefore
 * pays for the block allocation, the free-list threading across every cell, and
 * the page faults that threading triggers: on a 32k-cell order pool that is
 * ~1.5 MB and tens of microseconds, landing on one unlucky order rather than on
 * startup.
 * @c warm() moves all of it to construction, which is where the surrounding
 * code already assumes it happens.
 *
 * @par Free-list order
 * The warm-up returns its run through @c ordered_free, so the free list starts
 * in ascending address order and the first @c capacity acquires walk forward
 * through the block - the dense run the matching loop is meant to traverse.
 * After that @c release uses the unordered push, which is LIFO: the cell handed
 * out next is the one just released and therefore the hottest in cache. Both
 * orders are the right one for their moment, which is why the pool uses each
 * where it does.
 *
 * @par Why this and not @c boost::object_pool
 * @c object_pool::destroy goes through @c ordered_free, which walks the free
 * list to keep it sorted by address - O(free cells) on the cancel path, which
 * is exactly where an exchange cannot afford a walk. This wraps the plain
 * @c boost::pool and pays for the ordering once, at startup, and nowhere else.
 *
 * @par Address stability
 * Cells never move. Growing means chaining another block, not reallocating the
 * old one, so a pointer handed out by @c acquire stays valid until it is
 * released no matter how many orders arrive afterwards. That is the property
 * intrusive linking rests on: a level's FIFO is a chain of raw pointers between
 * cells, and one moved cell would break every link into it.
 *
 * @warning Not thread safe, by design - a matching core is one thread on one
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
	 * @brief A pool whose first block holds @p capacity cells, taken now.
	 * @param capacity Expected number of simultaneously live nodes. Size it to
	 *        worst-case book depth - @c high_water() is what tells you whether
	 *        you did.
	 * @param growth What happens past @p capacity. @c chained keeps the old
	 *        behaviour (another block, and the nodes stop being one dense run);
	 *        @c fixed refuses instead, trading rejected orders for flat
	 * latency.
	 */
	explicit basic_pool(std::size_t capacity = DEFAULT_CAPACITY,
						pool_growth growth   = pool_growth::chained)
		: capacity_(capacity > 0 ? capacity : DEFAULT_CAPACITY),
		  growth_(growth),
		  // The third argument is boost's ceiling on *one block*, not on the
		  // total: left at 0, next_size doubles after every block, so a book
		  // that overruns its hint twice allocates 4x the first block in a
		  // single call. Pinning it to the capacity keeps every block the size
		  // of the first, which is what makes a chained-growth stall bounded
		  // rather than compounding.
		  storage_(sizeof(T), capacity_, capacity_) {
		warm();
	}

	// Non-copyable, non-movable: live nodes point into the blocks.
	basic_pool(const basic_pool &)            = delete;
	basic_pool &operator=(const basic_pool &) = delete;
	basic_pool(basic_pool &&)                 = delete;
	basic_pool &operator=(basic_pool &&)      = delete;

	/**
	 * @brief Take a cell and construct a @c T in it from @p args.
	 * @return The constructed node, or @c nullptr if the pool is at capacity
	 *         under @c pool_growth::fixed, or could not obtain another block
	 *         under @c chained - the capacity error it is, reported rather than
	 *         thrown. An exception unwinding out of the matching loop would
	 *         leave a half-matched order behind, and there is nothing to catch
	 *         it on a path that has already printed trades.
	 * @warning Every caller must handle the null. It reaches the client as a
	 *          CANCELLED / BOOK_AT_CAPACITY outcome, not as a dropped order.
	 */
	template <typename... Args>
	[[nodiscard]] T *acquire(Args &&...args) noexcept {
		if (growth_ == pool_growth::fixed && live_ == capacity_) [[unlikely]]
			return nullptr;
		void *cell = storage_.malloc();
		if (cell == nullptr) [[unlikely]]
			return nullptr;
		// Two integer ops on a line the free-list pop just touched. What they
		// buy is high_water(), which is the number the capacity hint has to be
		// sized against and which nothing else in the book can report.
		++live_;
		high_water_ = std::max(live_, high_water_);
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
		assert(live_ > 0 && "release(): more cells returned than handed out");
		std::destroy_at(node);
		storage_.free(node); // unordered: pushes the cell, no walk
		--live_;
	}

	/// @brief Cells in the first block - the hint this pool was built with.
	[[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

	/// @brief Cells currently handed out and not yet released.
	[[nodiscard]] std::size_t live() const noexcept { return live_; }

	/**
	 * @brief The most cells ever live at once.
	 *
	 * The capacity-planning number, and the only one that survives the book
	 * emptying. A pool sized right reports a high water below @c capacity();
	 * one above it has chained a block (or refused an order) and the reading
	 * says by how much to raise the hint.
	 */
	[[nodiscard]] std::size_t high_water() const noexcept {
		return high_water_;
	}

	/**
	 * @brief Whether this pool ever went past its first block.
	 *
	 * Exact rather than inferred: the first block holds exactly @c capacity()
	 * cells, so a @c capacity()+1-th simultaneously live cell is precisely the
	 * event that chained a second one. True means the nodes are no longer one
	 * dense run and some @c acquire paid for a block inline - under
	 * @c pool_growth::fixed it means orders were refused instead.
	 */
	[[nodiscard]] bool overran() const noexcept {
		return high_water_ > capacity_;
	}

private:
	/**
	 * @brief Force the first block into existence, then give every cell back.
	 *
	 * @c ordered_malloc of the whole capacity is one contiguous run, which is
	 * what makes the block exist; boost threads a free list through every cell
	 * to build it, and that threading is what faults in the pages. Handing the
	 * run straight back through @c ordered_free leaves the list in ascending
	 * address order.
	 *
	 * A null run is startup OOM, and is left to @c acquire to report rather
	 * than thrown: a pool that could not warm still works, it just pays for the
	 * block later, which is exactly the old behaviour.
	 */
	void warm() noexcept {
		void *const run = storage_.ordered_malloc(capacity_);
		if (run == nullptr) [[unlikely]]
			return;
		storage_.ordered_free(run, capacity_);
	}

	// Declaration order is load-bearing: storage_ is constructed from
	// capacity_.
	std::size_t capacity_;
	pool_growth growth_;
	boost::pool<> storage_;
	std::size_t live_       = 0;
	std::size_t high_water_ = 0;
};

/// @brief The pool every resting order in a book is drawn from.
///
/// One pool per book, not one per level: a level is a transient thing that
/// appears when someone quotes a price and vanishes when the last order there
/// leaves, and giving each its own cells would scatter the nodes the matching
/// loop walks across as many blocks as there are prices.
using order_pool = basic_pool<resting_order>;

} // namespace exchange::engine::detail
