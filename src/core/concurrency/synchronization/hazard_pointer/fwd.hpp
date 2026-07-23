#pragma once
#include <cstddef>
#include <memory>

namespace concurrency::synchronization {

template <class T, class D = std::default_delete<T>>
class hazard_pointer_obj_base;

class hazard_pointer;
class hazard_pointer_domain;

hazard_pointer make_hazard_pointer();
hazard_pointer make_hazard_pointer(hazard_pointer_domain &);
void           swap(hazard_pointer &, hazard_pointer &) noexcept;

template <std::size_t N = 1>
class hazard_pointer_array;

} // namespace concurrency::synchronization
