#pragma once
#include "core_export.hpp"

#include <cstddef>
#include <memory>

namespace exchange::core::concurrency::synchronisation {

template <class T, class D = std::default_delete<T>>
class hazard_pointer_obj_base;

// Header-only (every member is inline in hazard_pointer.hpp; there is no
// hazard_pointer.cpp), so it must NOT be class-level exported: a dll-interface
// mark turns each consumer's inline-member uses into __imp_ references the DLL
// never emits (LNK2019). Consumers just compile their own inline copies.
class hazard_pointer;

class hazard_pointer_domain;

// CORE_EXPORT hazard_pointer make_hazard_pointer();
// CORE_EXPORT hazard_pointer make_hazard_pointer(hazard_pointer_domain &);
// Inline in hazard_pointer.hpp — not exported, for the same reason as the class.
void swap(hazard_pointer &, hazard_pointer &) noexcept;

template <std::size_t N = 1>
class hazard_pointer_array;

} // namespace exchange::core::concurrency::synchronisation
