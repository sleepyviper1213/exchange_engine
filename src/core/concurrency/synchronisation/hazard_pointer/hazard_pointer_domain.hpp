#pragma once
#include "fwd.hpp"
#include "hazard_pointer_obj.hpp"
#include "hazard_pointer_record.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>

namespace exchange::core::concurrency::synchronisation {
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

	hazard_pointer_domain(const hazard_pointer_domain &) = delete;

	hazard_pointer_domain &operator=(const hazard_pointer_domain &) = delete;

	// Runs at program exit (for the default domain) or when a caller-owned
	// domain dies. By construction no reader is active, so every remaining
	// retired object is unconditionally reclaimed, then the record stack is
	// freed.

	CORE_AUTOTEST_EXPORT ~hazard_pointer_domain();

	// Reclaim every retired object no record protects right now. Normally
	// invoked automatically by retire(); exposed so a caller can force a sweep
	// (e.g. in a quiescent phase or a test).
	CORE_AUTOTEST_EXPORT void cleanup() noexcept;

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
	CORE_EXPORT detail::hazard_pointer_record *acquire_slot();

	// Return a record to the free pool. The protection is cleared first so a
	// scan that observes the record as still active reads no stale pointer.
	static void release_slot(detail::hazard_pointer_record *s) noexcept {
		s->ptr.store(nullptr, std::memory_order_release);
		s->active.store(false, std::memory_order_release);
	}

	// Push a retired object and, if the backlog has grown past the threshold,
	// trigger a batched scan.
	CORE_EXPORT void retire(detail::hazard_pointer_obj *obj);

	// Reclaim past twice the live-record count plus a floor, so steady-state
	// memory is bounded by O(#hazard_pointers) while tiny workloads still
	// batch.
	[[nodiscard]] std::size_t threshold() const noexcept;

	// Detach the whole retired stack, free every object no record protects, and
	// push the survivors back. @c final skips the protection check entirely —
	// used only from the destructor, when no reader can exist.
	void reclaim(bool final) noexcept;

	static constexpr std::size_t kMinReclaim = 16;
};

// The process-wide domain used by make_hazard_pointer() and retire() when no
// explicit domain is named. MUST be a single shared instance, not thread_local:
// reclamation is only safe because a retiring thread scans the hazard pointers
// published by *every* other thread. A per-thread domain would let one thread
// free a node another thread is still protecting. Function-local static:
// constructed on first use, destroyed (draining all retired objects) at exit.
CORE_EXPORT hazard_pointer_domain &default_hazard_pointer_domain() noexcept;
} // namespace exchange::core::concurrency::synchronisation