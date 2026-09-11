#include "hazard_pointer_domain.hpp"

#include <boost/container/static_vector.hpp>

#include <algorithm>

namespace exchange::core::concurrency::synchronisation {

namespace {

/// @brief Protections the buffered scan can hold before it gives up on being
///        buffered at all.
///
/// One entry per slot publishing a live hazard pointer, which is at most one
/// per thread that has ever taken one. 64 covers every topology this engine is
/// built for - a partition per core, plus the feed, the logger and whatever
/// housekeeping - and costs 512 bytes of the reclaiming thread's stack. It is a
/// performance bound rather than a limit: past it the scan below stays correct
/// and only stops being O(log slots).
constexpr std::size_t FAST_SCAN_SLOTS = 64;

/// @brief The addresses a reclamation pass must not free, gathered on the
///        reclaiming thread's stack.
using protection_buffer =
	boost::container::static_vector<const void *, FAST_SCAN_SLOTS>;

/// @brief Is @p addr published in any slot from @p head onwards?
///
/// The unbuffered fallback: O(slots) per retired object where the sorted buffer
/// answers in O(log slots). Each slot is read with acquire for the same reason
/// the buffered walk does - the fence in reclaim() orders the publication, and
/// this is the load that observes it.
[[nodiscard]] bool is_protected(const detail::hazard_pointer_record *head,
								const void *addr) noexcept {
	for (const auto *s = head; s != nullptr;
		 s             = s->next.load(std::memory_order_relaxed))
		if (s->ptr.load(std::memory_order_acquire) == addr) return true;
	return false;
}

/// @brief Gather every live protection from @p head into @p out, sorted.
///
/// @return @c true when @p out holds them *all*, and is therefore a complete
///         answer a binary search may be run against. @c false when there were
///         more than it can hold - @p out is then not an answer at all and the
///         caller must ask @c is_protected per object instead.
///
/// @note The two outcomes are a performance choice, never a correctness one.
///       Truncating and searching the prefix would be the one unacceptable
///       option: a protection left out is a pointer freed while a reader holds
///       it, which is the use-after-free hazard pointers exist to prevent.
[[nodiscard]] bool
collect_protections(const detail::hazard_pointer_record *head,
					protection_buffer &out) noexcept {
	for (const auto *s = head; s != nullptr;
		 s             = s->next.load(std::memory_order_relaxed)) {
		const void *protection = s->ptr.load(std::memory_order_acquire);
		if (protection == nullptr) continue;
		// push_back past capacity is a precondition violation rather than an
		// error to catch, so the room is checked before the push, not after.
		if (out.size() == protection_buffer::static_capacity) return false;
		out.push_back(protection);
	}
	// Sorted so the caller's scan is a binary search rather than O(retired *
	// slots). The comparator is deliberately left defaulted: ranges::less on
	// pointers is the implementation-defined *strict total order*
	// ([range.cmp]), which these unrelated addresses need to satisfy sort's
	// strict-weak-ordering precondition. Do not respell this as a
	// `[](auto a, auto b){ return a < b; }` lambda - built-in `<` on pointers
	// into different objects is unspecified ([expr.rel]/5), so the ordering may
	// be intransitive and sort would run off the buffer. The matching
	// binary_search must stay defaulted for the same reason: both have to agree
	// on one order.
	std::ranges::sort(out);
	return true;
}

} // namespace

hazard_pointer_domain::~hazard_pointer_domain() {
	reclaim(detail::reclaim_mode::quiescent);
	const auto *s = slots_.load(std::memory_order_acquire);
	while (s != nullptr) {
		const auto *next = s->next.load(std::memory_order_relaxed);
		delete s; // NOLINT(cppcoreguidelines-owning-memory)
		s = next;
	}
}

void hazard_pointer_domain::cleanup() noexcept {
	reclaim(detail::reclaim_mode::concurrent);
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
	if (n >= threshold()) reclaim(detail::reclaim_mode::concurrent);
}

[[nodiscard]] std::size_t hazard_pointer_domain::threshold() const noexcept {
	return 2 * slot_count_.load(std::memory_order_relaxed) + MIN_RECLAIM;
}

void hazard_pointer_domain::reclaim(detail::reclaim_mode mode) noexcept {
	const bool scan_readers = mode == detail::reclaim_mode::concurrent;
	detail::hazard_pointer_obj *retired =
		retired_.exchange(nullptr, std::memory_order_acquire);
	retired_count_.store(0, std::memory_order_relaxed);
	if (retired == nullptr) return;

	// Asymmetric fence: pairs with the seq_cst fence in
	// hazard_pointer::try_protect so that any protection published before a
	// reader re-validated its load is visible to the scan below.
	std::atomic_thread_fence(std::memory_order_seq_cst);

	// Inline storage, not a vector: this function is noexcept and runs on the
	// reclamation path, so it may not reach an allocator - a throwing bad_alloc
	// here would terminate, and a nothrow one would leave the scan with no way
	// to answer.
	//
	// The slot list's head is read once and shared by both scans below, which
	// is what the buffered walk already did on its own. A slot pushed after
	// this load cannot be protecting anything on the retired list: the fence
	// above orders every protection published before a reader revalidated its
	// load, and a reader that had not published one yet has nothing to lose.
	const detail::hazard_pointer_record *const slots =
		slots_.load(std::memory_order_acquire);

	protection_buffer protecteds;
	// Whether `protecteds` holds *every* live protection. When it does, the
	// scan below is a binary search over it; when it does not - more live
	// protections than the fast path can hold - the buffer is abandoned and
	// each retired object is checked against the slot list directly instead.
	const bool buffered =
		scan_readers && collect_protections(slots, protecteds);

	detail::hazard_pointer_obj *survivors = nullptr;
	std::size_t kept                      = 0;
	while (retired != nullptr) {
		detail::hazard_pointer_obj *next = retired->next_;
		const bool protectedNow =
			scan_readers &&
			(buffered ? std::ranges::binary_search(protecteds,
												   retired->protected_addr_)
					  : is_protected(slots, retired->protected_addr_));
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
