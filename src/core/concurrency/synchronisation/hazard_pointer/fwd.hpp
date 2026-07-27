#pragma once
#include "core_export.hpp"

#include <cstddef>
#include <memory>

namespace exchange::core::concurrency::synchronisation {

template <class T, class D = std::default_delete<T>>
class hazard_pointer_obj_base;

class CORE_EXPORT hazard_pointer;
class CORE_EXPORT hazard_pointer_domain;

// CORE_EXPORT hazard_pointer make_hazard_pointer();
// CORE_EXPORT hazard_pointer make_hazard_pointer(hazard_pointer_domain &);
CORE_EXPORT void swap(hazard_pointer &, hazard_pointer &) noexcept;

template <std::size_t N = 1>
class hazard_pointer_array;

} // namespace exchange::core::concurrency::synchronisation
