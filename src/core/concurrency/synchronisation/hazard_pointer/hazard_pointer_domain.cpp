#include "hazard_pointer_domain.hpp"
#include <vector>
#include <algorithm>
namespace exchange::core::concurrency::synchronisation {

hazard_pointer_domain::~hazard_pointer_domain() {
	reclaim(reclaim_mode::quiescent);
	const auto *s = slots_.load(std::memory_order_acquire);
	while (s != nullptr) {
		const auto *next = s->next.load(std::memory_order_relaxed);
		delete s;
		s = next;
	}
}

void hazard_pointer_domain::cleanup() noexcept {
	reclaim(reclaim_mode::concurrent);
}

detail::hazard_pointer_record *hazard_pointer_domain::acquire_slot() {
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

void hazard_pointer_domain::retire(detail::hazard_pointer_obj *obj) {
	auto *head = retired_.load(std::memory_order_relaxed);
	do {
		obj->next_ = head;
	} while (!retired_.compare_exchange_weak(head,
											 obj,
											 std::memory_order_release,
											 std::memory_order_relaxed));
	const auto n = retired_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (n >= threshold()) reclaim(reclaim_mode::concurrent);
}

[[nodiscard]] std::size_t hazard_pointer_domain::threshold() const noexcept {
	return 2 * slot_count_.load(std::memory_order_relaxed) + MIN_RECLAIM;
}

void hazard_pointer_domain::reclaim(reclaim_mode mode) noexcept {
	const bool scan_readers = mode == reclaim_mode::concurrent;
	detail::hazard_pointer_obj *retired =
		retired_.exchange(nullptr, std::memory_order_acquire);
	retired_count_.store(0, std::memory_order_relaxed);
	if (retired == nullptr) return;

	// Asymmetric fence: pairs with the seq_cst fence in
	// hazard_pointer::try_protect so that any protection published before a
	// reader re-validated its load is visible to the scan below.
	std::atomic_thread_fence(std::memory_order_seq_cst);

	// TODO: May throw when allocation failed
	std::vector<const void *> protecteds;
	if (scan_readers) {
		for (auto *s = slots_.load(std::memory_order_acquire); s != nullptr;
			 s       = s->next.load(std::memory_order_relaxed)) {
			if (const void *p = s->ptr.load(std::memory_order_acquire))
				protecteds.push_back(p);
		}
		// Sorted so the scan below is a binary search rather than O(retired *
		// slots). The comparator is deliberately left defaulted: ranges::less
		// on pointers is the implementation-defined *strict total order*
		// ([range.cmp]), which these unrelated addresses need to satisfy
		// sort's strict-weak-ordering precondition. Do not respell this as a
		// `[](auto a, auto b){ return a < b; }` lambda — built-in `<` on
		// pointers into different objects is unspecified ([expr.rel]/5), so
		// the ordering may be intransitive and sort would run off the buffer.
		// The matching binary_search below must stay defaulted for the same
		// reason: both have to agree on one order.
		std::ranges::sort(protecteds);
	}

	detail::hazard_pointer_obj *survivors = nullptr;
	std::size_t kept                      = 0;
	while (retired != nullptr) {
		detail::hazard_pointer_obj *next = retired->next_;
		const bool protectedNow =
			scan_readers &&
			std::ranges::binary_search(protecteds, retired->protected_addr_);
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
		} while (!retired_.compare_exchange_weak(head,
												 survivors,
												 std::memory_order_release,
												 std::memory_order_relaxed));
		retired_count_.fetch_add(kept, std::memory_order_relaxed);
	}
}

hazard_pointer_domain &default_hazard_pointer_domain() noexcept {
	static hazard_pointer_domain domain;
	return domain;
}

} // namespace exchange::core::concurrency::synchronisation
