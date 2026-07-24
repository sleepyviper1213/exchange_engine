#pragma once

#include "concurrency/synchronization/hazard_pointer.hpp"

#include <atomic>
#include <cstddef>
#include <new>

namespace memory {

/**
 * @brief Intrusive lock-free stack of free blocks, made ABA-safe with hazard
 * pointers instead of a version tag.
 *
 * The alternative reclamation strategy to @c memory::tagged::FreeList, kept
 * behind @c memory::hazard so the two can be benchmarked head to head. It exists
 * to measure the cost of hazard-pointer protection against the near-free tagged
 * pointer on this exact workload — the tagged version is the inline-namespace
 * default because it is expected to win here (fixed blocks, stable addresses, no
 * reclamation needed).
 *
 * @par How the hazard pointers make it ABA-safe
 * A hazard pointer published on the popped head only guards *reclamation*, not
 * the CAS identity, so — exactly as in @c memory::pool::freelist — it
 * closes the ABA window only because recycling is routed *through* retirement: a
 * freed block is @c retire()d, and a reclaim deleter (@c Recycler) is what pushes
 * it back onto the free stack, once no hazard pointer still protects it. A block
 * a concurrent @c pop is mid-flight on therefore cannot reappear at the head, so
 * the head never transitions A → … → A within one pop's critical section.
 *
 * @note Each block hosts a @c Node — a retirable hazard-pointer object — so the
 * minimum block size (@c kMinBlockBytes) is larger than the tagged version's. A
 * block's storage holds a @c Node while free or reclamation-pending and raw
 * caller data while checked out.
 *
 * @par Threading contract
 * @c push and @c pop are safe from any number of threads concurrently.
 */
namespace hazard {

namespace sync = concurrency::synchronization;

class free_list {
	struct Node; // defined below; the recycler only needs the pointer type

	/// Reclaim deleter handed to @c retire(): instead of freeing the node it
	/// pushes it back onto the free stack. Because reclamation is
	/// hazard-pointer-gated, this is what keeps a block in flight from
	/// reappearing at the head and causing ABA.
	struct Recycler {
		std::atomic<Node *> *head = nullptr;
		void operator()(Node *n) const noexcept; // defined after Node is complete
	};

	struct Node : sync::hazard_pointer_obj_base<Node, Recycler> {
		std::atomic<Node *> next{nullptr};
	};

public:
	/// Smallest block able to host a retirable node.
	static constexpr std::size_t kMinBlockBytes = sizeof(Node);

	free_list() noexcept                   = default;
	free_list(const free_list &)            = delete;
	free_list &operator=(const free_list &) = delete;

	/// @brief Return a block to the list (block must be >= @c kMinBlockBytes).
	void push(void *block) noexcept {
		// Begin a node's lifetime in the block, then retire it — not push it —
		// so it re-enters the free stack only through the gated recycler.
		auto *node = ::new (block) Node();
		node->retire(domain_, Recycler{&head_});
	}

	/// @brief Pop a block for reuse, or @c nullptr if the list is empty.
	[[nodiscard]] void *pop() noexcept {
		auto hp    = sync::make_hazard_pointer(domain_);
		Node *node = try_pop(hp);
		if (node == nullptr) {
			// The free stack looks empty, but freed blocks may still be
			// reclamation-pending; force a sweep and retry once.
			domain_.cleanup();
			node = try_pop(hp);
			if (node == nullptr) return nullptr;
		}
		// Node is trivially destructible, so the next push()'s placement-new
		// reuses this storage without an explicit destroy; hand back raw bytes.
		return node;
	}

private:
	/// One hazard-pointer-protected Treiber pop attempt loop.
	Node *try_pop(sync::hazard_pointer &hp) noexcept {
		while (true) {
			Node *old = hp.protect(head_);
			if (old == nullptr) return nullptr;
			// old is protected, so it cannot be reclaimed and re-pushed; its
			// next is a stable, safe read.
			Node *next = old->next.load(std::memory_order_acquire);
			if (head_.compare_exchange_weak(old,
											next,
											std::memory_order_acquire,
											std::memory_order_relaxed)) {
				hp.reset_protection();
				return old;
			}
		}
	}

	static void treiber_push(std::atomic<Node *> &head, Node *n) noexcept {
		Node *cur = head.load(std::memory_order_relaxed);
		do {
			n->next.store(cur, std::memory_order_relaxed);
		} while (!head.compare_exchange_weak(cur,
											 n,
											 std::memory_order_release,
											 std::memory_order_relaxed));
	}

	std::atomic<Node *> head_{nullptr};
	/// Freelist-private domain: freed blocks must be reclaimed against *this*
	/// list's hazard pointers, and the process-wide domain would reclaim at
	/// program exit long after the list (and its arena storage) is gone.
	sync::hazard_pointer_domain domain_;
};

// Defined out of line: a nested class's member-function body is not a
// complete-class context of the enclosing class, so Node must already be
// complete here (and this is a concrete class, so there is no template
// instantiation to defer the check to).
inline void free_list::Recycler::operator()(Node *n) const noexcept {
	free_list::treiber_push(*head, n);
}

} // namespace hazard
} // namespace memory
