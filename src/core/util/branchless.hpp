#pragma once

#include <climits>
#include <concepts>

namespace exchange::core::util {
template <std::integral T>
[[nodiscard]] constexpr T abs_of(T v) noexcept {
	const T mask = v >> (sizeof(T) * CHAR_BIT - 1);
	return (v ^ mask) - mask;
}
} // namespace exchange::core::util
