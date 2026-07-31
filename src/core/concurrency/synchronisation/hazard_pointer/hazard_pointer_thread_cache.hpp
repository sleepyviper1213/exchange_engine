#pragma once
#include "core_export.hpp" // CORE_EXPORT (generated)
#include "hazard_pointer_record.hpp"

#include <array>

namespace exchange::core::concurrency::synchronisation::detail {

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
	static constexpr std::size_t CAPACITY = 8;

	hazard_pointer_thread_cache() = default;

	hazard_pointer_thread_cache(hazard_pointer_thread_cache &&) = delete;
	hazard_pointer_thread_cache &
	operator=(hazard_pointer_thread_cache &&)                        = delete;
	hazard_pointer_thread_cache(const hazard_pointer_thread_cache &) = delete;
	hazard_pointer_thread_cache &
	operator=(const hazard_pointer_thread_cache &) = delete;

	CORE_AUTOTEST_EXPORT ~hazard_pointer_thread_cache();

	// Take a recycled record, or nullptr if the stash is empty.
	CORE_EXPORT hazard_pointer_record *pop() noexcept;

	// Stash a record for reuse; false if the stash is full (caller should then
	// return it to the domain's free pool instead).
	CORE_EXPORT bool push(hazard_pointer_record *record) noexcept;

private:
	// Value-initialized so that, even if a broken toolchain fails to run this
	// object's constructor for a std::thread-created thread, a zeroed block is
	// still a valid empty cache (count_ == 0, no dangling record pointers).
	std::array<hazard_pointer_record *, CAPACITY> records_;
	std::size_t count_ = 0;
};

CORE_EXPORT hazard_pointer_thread_cache &default_thread_cache() noexcept;

} // namespace exchange::core::concurrency::synchronisation::detail
