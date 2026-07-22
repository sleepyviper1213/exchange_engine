#pragma once
#include "fwd.hpp"
#include "hazard_pointer_obj.hpp"
#include "hazard_pointer_record.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <vector>

namespace core::synchronisation {

namespace detail {
class hazard_pointer_thread_cache;
} // namespace detail

// Owns the two data structures behind hazard-pointer reclamation: the stack of
// hazard-pointer records (one per live hazard_pointer, reused across their
// lifetimes) and the stack of retired-but-not-yet-freed objects. Reclamation is
// batched: retiring an object past a growth threshold triggers a scan that
// frees every retired object no record currently protects.
//
// All public operations are lock-free and safe to call from any thread.
class hazard_pointer_domain {
public:
	hazard_pointer_domain() = default;

	hazard_pointer_domain(const hazard_pointer_domain &)            = delete;
	hazard_pointer_domain &operator=(const hazard_pointer_domain &) = delete;

	// Runs at program exit (for the default domain) or when a caller-owned
	// domain dies. By construction no reader is active, so every remaining
	// retired object is unconditionally reclaimed, then the record stack is
	// freed.
	~hazard_pointer_domain() {
		reclaim(/*final=*/true);
		const auto *s = slots_.load(std::memory_order_acquire);
		while (s != nullptr) {
			const auto *next = s->next.load(std::memory_order_relaxed);
			delete s;
			s = next;
		}
	}

	// Reclaim every retired object no record protects right now. Normally
	// invoked automatically by retire(); exposed so a caller can force a sweep
	// (e.g. in a quiescent phase or a test).
	void cleanup() noexcept { reclaim(/*final=*/false); }

private:
	friend class hazard_pointer;

	template <class, class>
	friend class hazard_pointer_obj_base;

	// The factory functions borrow a record via the private acquire_slot().
	friend hazard_pointer make_hazard_pointer();

	friend hazard_pointer make_hazard_pointer(hazard_pointer_domain &);

	// The thread cache returns records to the free pool via release_slot().
	friend class detail::hazard_pointer_thread_cache;

	std::atomic<detail::hazard_pointer_record *> slots_{nullptr};
	std::atomic<detail::hazard_pointer_obj *> retired_{nullptr};
	std::atomic<std::size_t> retired_count_{0};
	std::atomic<std::size_t> slot_count_{0};

	// Hand out a hazard-pointer record: reuse an inactive one if any, otherwise
	// grow the stack. The record is published into the stack before it is
	// returned, so a concurrent scan can never miss a slot that is about to
	// protect something.
	detail::hazard_pointer_record *acquire_slot() {
		for (auto *s = slots_.load(std::memory_order_acquire); s != nullptr;
			 s       = s->next.load(std::memory_order_relaxed)) {
			bool expected = false;
			if (!s->active.load(std::memory_order_relaxed) &&
				s->active.compare_exchange_strong(expected,
												  true,
												  std::memory_order_acquire,
												  std::memory_order_relaxed)) {
				return s;
			}
		}

		auto *s = new detail::hazard_pointer_record;
		s->active.store(true, std::memory_order_relaxed);
		auto *head = slots_.load(std::memory_order_relaxed);
		do {
			s->next.store(head, std::memory_order_relaxed);
		} while (!slots_.compare_exchange_weak(head,
											   s,
											   std::memory_order_release,
											   std::memory_order_relaxed));
		slot_count_.fetch_add(1, std::memory_order_relaxed);
		return s;
	}

	// Return a record to the free pool. The protection is cleared first so a
	// scan that observes the record as still active reads no stale pointer.
	static void release_slot(detail::hazard_pointer_record *s) noexcept {
		s->ptr.store(nullptr, std::memory_order_release);
		s->active.store(false, std::memory_order_release);
	}

	// Push a retired object and, if the backlog has grown past the threshold,
	// trigger a batched scan.
	void retire(detail::hazard_pointer_obj *obj) {
		auto *head = retired_.load(std::memory_order_relaxed);
		do {
			obj->next_ = head;
		} while (!retired_.compare_exchange_weak(head,
												 obj,
												 std::memory_order_release,
												 std::memory_order_relaxed));
		const auto n =
			retired_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
		if (n >= threshold()) reclaim(/*final=*/false);
	}

	// Reclaim past twice the live-record count plus a floor, so steady-state
	// memory is bounded by O(#hazard_pointers) while tiny workloads still
	// batch.
	[[nodiscard]] std::size_t threshold() const noexcept {
		return 2 * slot_count_.load(std::memory_order_relaxed) + kMinReclaim;
	}

	// Detach the whole retired stack, free every object no record protects, and
	// push the survivors back. @c final skips the protection check entirely —
	// used only from the destructor, when no reader can exist.
	void reclaim(bool final) noexcept {
		detail::hazard_pointer_obj *retired =
			retired_.exchange(nullptr, std::memory_order_acquire);
		retired_count_.store(0, std::memory_order_relaxed);
		if (retired == nullptr) return;

		// Asymmetric fence: pairs with the seq_cst fence in
		// hazard_pointer::try_protect so that any protection published before a
		// reader re-validated its load is visible to the scan below.
		std::atomic_thread_fence(std::memory_order_seq_cst);

		std::vector<const void *> protecteds;
		if (!final) {
			for (auto *s = slots_.load(std::memory_order_acquire); s != nullptr;
				 s       = s->next.load(std::memory_order_relaxed)) {
				if (const void *p = s->ptr.load(std::memory_order_acquire))
					protecteds.push_back(p);
			}
			std::ranges::sort(protecteds);
		}

		detail::hazard_pointer_obj *survivors = nullptr;
		std::size_t kept                      = 0;
		while (retired != nullptr) {
			detail::hazard_pointer_obj *next = retired->next_;
			const bool protectedNow =
				!final && std::ranges::binary_search(protecteds,
													 retired->protected_addr_);
			if (protectedNow) {
				retired->next_ = survivors;
				survivors      = retired;
				++kept;
			} else {
				retired->reclaim_(retired);
			}
			retired = next;
		}

		if (survivors != nullptr) {
			detail::hazard_pointer_obj *tail = survivors;
			while (tail->next_ != nullptr) tail = tail->next_;
			auto *head = retired_.load(std::memory_order_relaxed);
			do {
				tail->next_ = head;
			} while (
				!retired_.compare_exchange_weak(head,
												survivors,
												std::memory_order_release,
												std::memory_order_relaxed));
			retired_count_.fetch_add(kept, std::memory_order_relaxed);
		}
	}

	static constexpr std::size_t kMinReclaim = 16;
};

// The process-wide domain used by make_hazard_pointer() and retire() when no
// explicit domain is named. MUST be a single shared instance, not thread_local:
// reclamation is only safe because a retiring thread scans the hazard pointers
// published by *every* other thread. A per-thread domain would let one thread
// free a node another thread is still protecting. Function-local static:
// constructed on first use, destroyed (draining all retired objects) at exit.
inline hazard_pointer_domain &default_hazard_pointer_domain() noexcept {
	static hazard_pointer_domain domain;
	return domain;
}

namespace detail {

// A small per-thread stash of default-domain records. Acquiring and releasing a
// hazard pointer is overwhelmingly a balanced, same-thread pair, so instead of
// walking the domain's shared record stack (and CASing the @c active flag)
// every time, each thread keeps a handful of records it owns and recycles them
// with no atomics at all. A cached record stays active (owned by this thread)
// but protects nothing — its @c ptr is cleared on release — so a concurrent
// scan simply skips it. On thread exit the stash is drained back to the
// domain's free pool so other threads can reuse the records.
//
// Scoped to the default domain only: records are domain-specific, and a single
// thread-local stash cannot safely mix records from several explicit domains,
// so make_hazard_pointer(domain) bypasses the cache entirely.
class hazard_pointer_thread_cache {
public:
	static constexpr std::size_t kCapacity = 8;

	hazard_pointer_thread_cache() = default;

	hazard_pointer_thread_cache(const hazard_pointer_thread_cache &) = delete;
	hazard_pointer_thread_cache &
	operator=(const hazard_pointer_thread_cache &) = delete;

	~hazard_pointer_thread_cache() {
		// The default domain is a static that outlives every thread cache (each
		// cache is only ever populated after default_hazard_pointer_domain()
		// has run), so returning records here is safe.
		assert(count_ <= kCapacity);
		auto &domain = default_hazard_pointer_domain();
		for (std::size_t i = 0; i < count_; ++i)
			domain.release_slot(records_[i]);
	}

	// Take a recycled record, or nullptr if the stash is empty.
	hazard_pointer_record *pop() noexcept {
		// Guards the count_ invariant: a corrupted count_ (e.g. a thread whose
		// thread_local storage was not initialized) would otherwise index out of
		// bounds instead of failing loudly.
		assert(count_ <= kCapacity);
		return count_ == 0 ? nullptr : records_[--count_];
	}

	// Stash a record for reuse; false if the stash is full (caller should then
	// return it to the domain's free pool instead).
	bool push(hazard_pointer_record *record) noexcept {
		assert(record != nullptr);
		assert(count_ <= kCapacity);
		if (count_ == kCapacity) return false;
		records_[count_++] = record;
		return true;
	}

private:
	// Value-initialized so that, even if a broken toolchain fails to run this
	// object's constructor for a std::thread-created thread, a zeroed block is
	// still a valid empty cache (count_ == 0, no dangling record pointers).
	hazard_pointer_record *records_[kCapacity]{};
	std::size_t            count_ = 0;
};

inline hazard_pointer_thread_cache &default_thread_cache() noexcept {
	thread_local hazard_pointer_thread_cache cache;
	return cache;
}

} // namespace detail

} // namespace core::synchronisation
