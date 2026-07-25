#pragma once
#include "concurrency/synchronisation/hazard_pointer.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace memory::pool {

namespace sync = concurrency::synchronisation;

/**
 * @brief Lock-free, bounded object pool (freelist) with ABA-safe node recycling
 * via hazard pointers.
 *
 * A fixed set of @c capacity nodes is allocated once, up front, and recycled
 * forever after: @c acquire() constructs a @c T in a free node and hands it
 * out,
 * @c release() destroys the @c T and returns the node to circulation. No
 * allocation happens on the hot path, which is why this exists — a matching
 * engine must not call into the general allocator to mint and reclaim orders.
 *
 * @note The @c pool namespace sets this apart from @c memory::tagged::free_list
 * /
 * @c memory::hazard::free_list: those recycle raw same-size blocks (push/pop a
 * @c void*); this is a typed object pool that constructs and destroys a @c T
 * (acquire/release). The similar names are a hazard — do not conflate them.
 *
 * @par Why hazard pointers, and why they alone are not enough
 * The free nodes form a lock-free Treiber stack, so @c acquire() is a
 * compare-and-swap pop and the classic ABA hazard applies: a thread reads
 * @c head==A and @c A->next, is stalled, and by the time it CASes, @c A has
 * been popped and pushed again with a different @c next — the CAS wrongly
 * succeeds. A hazard pointer published on @c A does @b not by itself close this
 * window: a hazard pointer guards *reclamation* (it stops the node's memory
 * being freed), not the CAS *identity*. In a naive freelist that pushes a
 * released node straight back, nothing is ever freed, so the guard is vacuous
 * and ABA remains.
 *
 * The fix is to make recycling itself pass through the hazard-pointer domain: a
 * released node is @c retire()d, not pushed, and a custom reclaim deleter (see
 * @c recycler) is what returns it to the free stack. Reclamation is gated on no
 * hazard pointer protecting the node, so a node that any @c acquire() is
 * mid-pop on cannot re-enter the free stack until that acquirer clears its
 * hazard pointer. The head therefore cannot transition A → … → A within a
 * single acquire's critical section, and ABA is impossible. This is the whole
 * point of the design: the hazard pointer on the popped head, combined with
 * recycle-only-through-retire, is what makes the CAS safe.
 *
 * @tparam T Pooled element type; must be nothrow-destructible (release() is
 * @c noexcept and destroys the element in place).
 *
 * @par Threading contract
 * @c acquire() and @c release() are lock-free and callable from any number of
 * threads concurrently. @c quiesce() and the destructor require that no other
 * thread is touching the pool.
 *
 * @invariant Every node is in exactly one of three states: on the free stack,
 * checked out (a live @c T constructed in it), or retired-pending (released,
 * its
 * @c T already destroyed, awaiting reclamation back onto the free stack).
 * @invariant A node re-enters the free stack only through reclamation, which is
 * hazard-pointer-gated — the property that makes @c acquire() ABA-safe.
 *
 * @note Reclamation is batched by the domain, so a just-released node is not
 * instantly available again: it lingers in the retired-pending state until a
 * sweep returns it. @c acquire() can thus report exhaustion (returns @c
 * nullptr) while released nodes are still draining back. Call @c quiesce() in a
 * quiescent phase to force them home.
 */
template <class T>
class free_list {
	static_assert(std::is_nothrow_destructible_v<T>,
				  "release() destroys T in place inside a noexcept function");

	struct node; // defined below; the recycler only needs the pointer type

	/// Reclaim deleter handed to @c retire(). Rather than freeing the node it
	/// returns it to the free stack — and because @c retire() only reclaims a
	/// node no hazard pointer protects, this is exactly what keeps a node in
	/// flight from reappearing at the head and causing ABA (see class docs).
	struct recycler {
		std::atomic<node *> *free_head = nullptr;

		void operator()(node *n) const noexcept { push(*free_head, n); }
	};

	/// A pooled cell: the hazard-pointer base (so it can be retired), raw
	/// storage for one @c T whose lifetime is managed explicitly, and the
	/// intrusive free-stack link.
	struct node : sync::hazard_pointer_obj_base<node, recycler> {
		alignas(T) std::byte storage[sizeof(T)];
		std::atomic<node *> next{nullptr};

		[[nodiscard]] T *value() noexcept {
			return std::launder(reinterpret_cast<T *>(storage));
		}
	};

public:
	/**
	 * @brief Allocate @p capacity nodes and place them all on the free stack.
	 * @param capacity Fixed number of elements the pool can hand out at once.
	 * @post The pool holds @p capacity free nodes and never allocates again.
	 */
	explicit free_list(std::size_t capacity)
		: nodes_(std::make_unique<node[]>(capacity)), capacity_(capacity) {
		// Construction is single-threaded, so link the nodes directly — no
		// hazard pointer or CAS contention to worry about yet.
		for (std::size_t i = 0; i < capacity_; ++i)
			push(free_head_, &nodes_[i]);

		// The value pointer sits at a fixed offset inside every (identically
		// laid-out) node, so recover it once here and reuse it in from_value()
		// rather than recomputing per release().
		if (capacity_ > 0)
			value_offset_ = reinterpret_cast<std::byte *>(nodes_[0].value()) -
							reinterpret_cast<std::byte *>(&nodes_[0]);
	}

	free_list(const free_list &)            = delete;
	free_list &operator=(const free_list &) = delete;
	// A member domain and atomics make the pool non-movable; pin it in place.
	free_list(free_list &&)            = delete;
	free_list &operator=(free_list &&) = delete;

	/**
	 * @brief Reclaim all storage.
	 * @pre Quiescent: no thread is acquiring, releasing, or holding a
	 * checked-out element. Any @c T still checked out is the caller's to
	 * release first.
	 * @details Member destruction runs in reverse declaration order, so @c
	 * domain_ is torn down first: it reclaims every retired-pending node, whose
	 * recycler harmlessly pushes it onto @c free_head_ (still alive). @c nodes_
	 * is destroyed last, freeing the storage exactly once — the recycler never
	 * frees, so there is no double free.
	 */
	~free_list() = default;

	/**
	 * @brief Construct a @c T in a free node and hand it out.
	 * @pre Callable from any thread.
	 * @post On success a live @c T is returned and the pool has one fewer free
	 * node; on exhaustion nothing is constructed.
	 * @param args Constructor arguments forwarded to @c T.
	 * @return Pointer to the constructed element, or @c nullptr if no free node
	 * was available (see the batching note in the class docs).
	 */
	template <class... Args>
	[[nodiscard]] T *acquire(Args &&...args) {
		node *n = pop();
		if (n == nullptr) [[unlikely]] {
			// The free stack looks empty, but released nodes may still be in
			// the retired-pending state: reclamation is batched, and the
			// domain's threshold can exceed a small pool's capacity, so a sweep
			// might not have fired yet. Force one and retry once before
			// reporting exhaustion. This still recycles only through the
			// hazard-pointer gate, so the ABA guarantee is unaffected.
			domain_.cleanup();
			n = pop();
			if (n == nullptr) return nullptr; // genuinely exhausted
		}
		return std::construct_at(n->value(), std::forward<Args>(args)...);
	}

	/**
	 * @brief Destroy a checked-out element and return its node to the pool.
	 * @pre @p p was returned by @c acquire() on this pool and not yet released.
	 * Callable from any thread.
	 * @post The element is destroyed and its node is retired for ABA-safe
	 * recycling; it becomes acquirable again once no hazard pointer protects
	 * it.
	 * @param p The element to release; @c nullptr is a no-op.
	 */
	void release(T *p) noexcept {
		if (p == nullptr) return;
		std::destroy_at(p);
		// Retire — do not push directly. Reclamation returns the node via the
		// recycler once no acquirer protects it, which is what closes the ABA
		// window (see class docs).
		from_value(p)->retire(domain_, recycler{&free_head_});
	}

	/**
	 * @brief Force a reclamation sweep, returning released nodes to the pool.
	 * @details Thread-safe (it is the same gated sweep @c retire() runs
	 * internally), so it may be called under concurrent acquire/release. It
	 * only recovers nodes no hazard pointer currently protects; a node another
	 * thread is mid-acquire on stays pending until that thread clears its
	 * protection.
	 * @post Every released node not currently protected is back on the free
	 * stack. With no concurrent activity, the pool's full capacity is
	 * acquirable.
	 */
	void quiesce() noexcept { domain_.cleanup(); }

	/// @brief Total nodes owned by the pool (not the number currently free).
	[[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
	/// Treiber push, shared by the initial fill and the recycler.
	static void push(std::atomic<node *> &head, node *n) noexcept {
		node *cur = head.load(std::memory_order_relaxed);
		do {
			n->next.store(cur, std::memory_order_relaxed);
		} while (!head.compare_exchange_weak(cur,
											 n,
											 std::memory_order_release,
											 std::memory_order_relaxed));
	}

	/// Hazard-pointer-protected Treiber pop. Publishing a hazard pointer on the
	/// head before dereferencing it is what makes the recycling ABA-safe: the
	/// protected head cannot be reclaimed, so it cannot be recycled back to the
	/// head and change @c next underneath this CAS.
	node *pop() noexcept {
		auto hp = sync::make_hazard_pointer(domain_);
		while (true) {
			node *old = hp.protect(free_head_);
			if (old == nullptr) return nullptr; // empty
			// old is protected, so old->next is a stable, safe read: old cannot
			// be reclaimed and thus cannot be re-pushed with a different next.
			node *next = old->next.load(std::memory_order_acquire);
			if (free_head_.compare_exchange_weak(old,
												 next,
												 std::memory_order_acquire,
												 std::memory_order_relaxed)) {
				hp.reset_protection(); // we own old; stop protecting it
				return old;
			}
			// Lost the race; loop and protect the new head.
		}
	}

	/// Recover the owning node from a value pointer (the inverse of
	/// @c node::value), using the fixed intra-node offset established in the
	/// constructor.
	[[nodiscard]] node *from_value(T *p) const noexcept {
		return reinterpret_cast<node *>(reinterpret_cast<std::byte *>(p) -
										value_offset_);
	}

	std::unique_ptr<node[]> nodes_;   ///< owns storage; freed last
	std::size_t capacity_        = 0;
	std::ptrdiff_t value_offset_ = 0; ///< value pointer -> node, in bytes
	std::atomic<node *> free_head_{nullptr};
	/// Pool-private domain: a released node must be reclaimed against *this*
	/// pool's hazard pointers and returned to *this* pool. The process-wide
	/// default domain would reclaim at program exit, long after the pool is
	/// gone, and push into freed storage. Declared last so it is destroyed
	/// first (see the destructor).
	sync::hazard_pointer_domain domain_;
};

} // namespace memory::pool
