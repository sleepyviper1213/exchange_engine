#pragma once
// A borrowed predicate: the strict corner of function_ref, named.

#include "core/util/function_ref.hpp"

namespace exchange::core::util {
/// @brief A borrowed `bool(T)` that neither mutates its target nor throws.
template <typename T>
using predicate_ref = function_ref<bool(T) const noexcept>;

} // namespace exchange::core::util
