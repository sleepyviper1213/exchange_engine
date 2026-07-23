#pragma once
#include "fwd.hpp"

namespace concurrency::synchronization::detail {
// Type-erased base of every retirable object. Carries the intrusive link used
// by the domain's retired stack, the reclaim thunk that knows the object's
// concrete type, and the address a reader would publish to protect it (which
// is the most-derived pointer, not this base subobject).
class hazard_pointer_obj {
	// Fully qualified: an unqualified name here would befriend a phantom
	// detail::hazard_pointer_domain, not the real one in the parent namespace.
	friend class ::concurrency::synchronization::hazard_pointer_domain;
	template <class, class>
	friend class ::concurrency::synchronization::hazard_pointer_obj_base;

	hazard_pointer_obj *next_              = nullptr;
	void (*reclaim_)(hazard_pointer_obj *) = nullptr;
	const void *protected_addr_            = nullptr;

protected:
	hazard_pointer_obj() = default;

	// The reclamation bookkeeping is per-retirement, never copied: copying or
	// moving a live object must not carry another object's retired-list link.
	hazard_pointer_obj(const hazard_pointer_obj &) noexcept {}

	hazard_pointer_obj(hazard_pointer_obj &&) noexcept {}

	hazard_pointer_obj &operator=(const hazard_pointer_obj &) noexcept ;


	hazard_pointer_obj &operator=(hazard_pointer_obj &&) noexcept ;

	~hazard_pointer_obj() = default;
};
} // namespace concurrency::synchronization::detail