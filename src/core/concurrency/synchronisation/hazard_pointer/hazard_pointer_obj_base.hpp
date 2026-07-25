#pragma once
#include "hazard_pointer_obj.hpp"
#include "hazard_pointer_domain.hpp"

namespace concurrency::synchronisation {
// CRTP base that makes a type retirable through a hazard-pointer domain. A user
// type derives from it as its primary base:
//
//     struct node : hazard_pointer_obj_base<node> { ... };
//
// so that the address a reader publishes to protect (the @c node*) coincides
// with the base subobject the domain tracks. @c retire() hands the object to
// the default domain, which frees it once no reader protects it.
template <class T, class D>
class hazard_pointer_obj_base : public detail::hazard_pointer_obj {
public:
	// Transfer ownership of *this to a hazard-pointer domain. After this call
	// the object may be destroyed at any time by a reclaiming thread, so the
	// caller must hold no further references. @c retire() must be called at
	// most once per object, and the domain must be the same one whose hazard
	// pointers protect this object.
	void retire(D d = D()) noexcept {
		retire_into(default_hazard_pointer_domain(), std::move(d));
	}

	// Retire into an explicit domain (pair with make_hazard_pointer(domain)).
	void retire(hazard_pointer_domain &domain, D d = D()) noexcept {
		retire_into(domain, std::move(d));
	}

protected:
	hazard_pointer_obj_base()                                = default;
	hazard_pointer_obj_base(const hazard_pointer_obj_base &) = default;
	hazard_pointer_obj_base(hazard_pointer_obj_base &&)      = default;
	hazard_pointer_obj_base &
	operator=(const hazard_pointer_obj_base &)                     = default;
	hazard_pointer_obj_base &operator=(hazard_pointer_obj_base &&) = default;
	~hazard_pointer_obj_base()                                     = default;

private:
	void retire_into(hazard_pointer_domain &domain, D d) noexcept {
		deleter_ = std::move(d);
		reclaim_ = [](hazard_pointer_obj *p) noexcept {
			auto *self = static_cast<hazard_pointer_obj_base *>(p);
			self->deleter_(static_cast<T *>(self));
		};
		// The pointer a reader protects is the most-derived T*, which need not
		// equal this base subobject under multiple inheritance; record it so
		// the scan compares like for like.
		protected_addr_ = static_cast<const void *>(static_cast<T *>(this));
		domain.retire(this);
	}

	[[no_unique_address]] D deleter_;
};
} // namespace concurrency::synchronisation