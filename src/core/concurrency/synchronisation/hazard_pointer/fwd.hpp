#pragma once

#include <cstddef>
#include <memory>

namespace exchange::core::concurrency::synchronisation {

template <class T, class D = std::default_delete<T>>
class hazard_pointer_obj_base;

class hazard_pointer;

class hazard_pointer_domain;

template <std::size_t N = 1>
class hazard_pointer_array;

} // namespace exchange::core::concurrency::synchronisation
