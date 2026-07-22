#pragma once

#include <atomic>
#include <new>

namespace core::synchronisation::detail {
// A record is created once and lives for the whole
// lifetime of its owning domain; it is never freed while the domain is alive,
// only handed back to a free pool (via @c active) when its owning
// hazard_pointer is destroyed. @c ptr holds the address a reader currently
// protects, or nullptr when the slot protects nothing.
struct alignas(std::hardware_destructive_interference_size)
	hazard_pointer_record {
	std::atomic<const void *> ptr{nullptr};
	std::atomic<bool> active{false};
	// Link in the domain's intrusive slot stack. Written once, before the slot
	// is CAS-published into the stack, then immutable — plain load is safe for
	// any thread that has acquire-observed the stack head.
	std::atomic<hazard_pointer_record *> next{nullptr};
};
} // namespace core::synchronisation::detail