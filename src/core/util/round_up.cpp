#include "round_up.hpp"

#include <bit>
#include <cassert>
#include <new>

namespace exchange::core::util{
std::size_t round_up(std::size_t n, std::align_val_t multiple) noexcept {
	const auto m = static_cast<size_t>(multiple);
	assert(std::has_single_bit(m) && "multiple must be a power of two");
	return (n + m - 1) & ~(m - 1);
}
} // namespace exchange::util
