#pragma once
#include "fwd.hpp"
#include "hazard_pointer_domain.hpp"
#include "hazard_pointer_thread_cache.hpp"

#include <atomic>
#include <cstddef>
#include <utility>

namespace exchange::core::concurrency::synchronisation {

// An RAII handle to a single hazard-pointer record borrowed from a domain. A
// default-constructed handle is empty and owns nothing; make_hazard_pointer()
// yields an owning one. Move-only — a record has exactly one owner at a time —
// and returns its record to the domain's free pool on destruction.
class hazard_pointer {
public:
	hazard_pointer() noexcept = default;

	hazard_pointer(const hazard_pointer &)            = delete;
	hazard_pointer &operator=(const hazard_pointer &) = delete;

	hazard_pointer(hazard_pointer &&other) noexcept
		: slot_(other.slot_), domain_(other.domain_) {
		other.slot_ = nullptr;
	}

	hazard_pointer &operator=(hazard_pointer &&other) noexcept {
		if (this != &other) {
			release();
			slot_       = other.slot_;
			domain_     = other.domain_;
			other.slot_ = nullptr;
		}
		return *this;
	}

	~hazard_pointer() { release(); }

	[[nodiscard]]
	bool empty() const noexcept {
		return slot_ == nullptr;
	}

	// Publish and validate protection of whatever @c src points to, retrying
	// until the published pointer still matches @c src, and return it. The
	// returned pointer is safe to dereference until this handle protects a
	// different pointer or is destroyed.
	template <class T>
	T *protect(const std::atomic<T *> &src) noexcept {
		T *ptr = src.load(std::memory_order_relaxed);
		while (!try_protect(ptr, src)) {
			// keep retrying.
		}
		return ptr;
	}

	// One attempt at protecting @c ptr: publish it, then re-read @c src. On
	// success @c ptr is protected and left unchanged. On failure @c ptr is
	// updated to the freshly observed value so the caller can retry.
	template <class T>
	bool try_protect(T *&ptr, const std::atomic<T *> &src) noexcept {
		reset_protection(ptr);
		// Pairs with the domain's seq_cst fence before it scans records: the
		// publish above must be globally ordered before the reload below, so a
		// reclaimer either sees our protection or we see its unlink.
		std::atomic_thread_fence(std::memory_order_seq_cst);
		T *reloaded = src.load(std::memory_order_acquire);
		if (reloaded == ptr) [[likely]]
			return true;
		ptr = reloaded;
		return false;
	}

	// Publish @c ptr as protected without validating it against any source.
	// Callers that already know the pointer is stable (or want to clear) use
	// this directly.
	template <class T>
	void reset_protection(const T *ptr) noexcept {
		slot_->ptr.store(static_cast<const void *>(ptr),
						 std::memory_order_release);
	}

	void reset_protection(std::nullptr_t = nullptr) noexcept {
		slot_->ptr.store(nullptr, std::memory_order_release);
	}

	void swap(hazard_pointer &other) noexcept {
		std::swap(slot_, other.slot_);
		std::swap(domain_, other.domain_);
	}

private:
	friend hazard_pointer make_hazard_pointer();
	friend hazard_pointer make_hazard_pointer(hazard_pointer_domain &);

	hazard_pointer(hazard_pointer_domain &domain,
				   detail::hazard_pointer_record *slot) noexcept
		: slot_(slot), domain_(&domain) {}

	// Give up this handle's record. Default-domain records recycle through the
	// thread cache (no shared-state contention); anything else, or a full
	// cache, goes straight back to the owning domain's free pool.
	void release() noexcept {
		if (slot_ == nullptr) return;
		if (domain_ == &default_hazard_pointer_domain()) {
			slot_->ptr.store(nullptr, std::memory_order_release);
			if (detail::default_thread_cache().push(slot_)) {
				slot_ = nullptr;
				return;
			}
		}
		domain_->release_slot(slot_);
		slot_ = nullptr;
	}

	detail::hazard_pointer_record *slot_ = nullptr;
	hazard_pointer_domain *domain_       = nullptr;
};

// Acquire a hazard pointer from the process-wide default domain, reusing a
// record from this thread's cache when one is available.
inline hazard_pointer make_hazard_pointer() {
	auto &domain = default_hazard_pointer_domain();
	auto *record = detail::default_thread_cache().pop();
	if (record == nullptr) record = domain.acquire_slot();
	return {domain, record};
}

// Acquire a hazard pointer from an explicit domain.
inline hazard_pointer make_hazard_pointer(hazard_pointer_domain &domain) {
	return {domain, domain.acquire_slot()};
}

inline void swap(hazard_pointer &a, hazard_pointer &b) noexcept { a.swap(b); }


} // namespace exchange::core::concurrency::synchronisation
