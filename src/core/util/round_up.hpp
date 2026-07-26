#pragma once
#include <cstddef>
#include <new>

namespace exchange::core::util{
std::size_t round_up(std::size_t n, std::align_val_t multiple) noexcept;
} // namespace exchange::util
