#include "hazard_pointer_thread_cache.hpp"

#include "hazard_pointer_domain.hpp"

#include <cassert>

namespace exchange::core::concurrency::synchronisation::detail {
hazard_pointer_thread_cache::~hazard_pointer_thread_cache() {
	// The default domain is a static that outlives every thread cache (each
	// cache is only ever populated after default_hazard_pointer_domain()
	// has run), so returning records here is safe.
	assert(count_ <= CAPACITY);
	auto &domain = default_hazard_pointer_domain();
	for (std::size_t i = 0; i < count_; ++i) domain.release_slot(records_[i]);
}

hazard_pointer_record *hazard_pointer_thread_cache::pop() noexcept {
	// Guards the count_ invariant: a corrupted count_ (e.g. a thread whose
	// thread_local storage was not initialized) would otherwise index out of
	// bounds instead of failing loudly.
	assert(count_ <= CAPACITY);
	return count_ == 0 ? nullptr : records_[--count_];
}

bool hazard_pointer_thread_cache::push(hazard_pointer_record *record) noexcept {
	assert(record != nullptr);
	assert(count_ <= CAPACITY);
	if (count_ == CAPACITY) return false;
	records_[count_++] = record;
	return true;
}

hazard_pointer_thread_cache &default_thread_cache() noexcept {
	thread_local hazard_pointer_thread_cache cache;
	return cache;
}
} // namespace exchange::core::concurrency::synchronisation::detail
