#pragma once

#ifdef __cpp_lib_start_lifetime_as
#include <memory>
#else
#include <cstring>
#include <new>
#include <type_traits>
#endif

namespace exchange::core::util {
#ifdef __cpp_lib_start_lifetime_as
using std::start_lifetime_as;
using std::start_lifetime_as_array;
#else
// Source - https://stackoverflow.com/a/76794371
// Posted by Jan Schultke, modified by community. See post 'Timeline' for change
// history Retrieved 2026-08-19, License - CC BY-SA 4.0
template <class T>
	requires (std::is_trivially_copyable_v<T> && std::is_implicit_lifetime_v<T>)
[[nodiscard]] T *start_lifetime_as(void *p) noexcept {
	return std::launder(static_cast<T *>(std::memmove(p, p, sizeof(T))));
}

// Posted by user17732522, modified by community. See post 'Timeline' for change
// history Retrieved 2026-07-07, License - CC BY-SA 4.0
template <class T>
	requires (std::is_trivially_copyable_v<T>)
[[nodiscard]]
T *start_lifetime_as_array(void *p, std::size_t n) noexcept {
	if (n == 0) return static_cast<T *>(p);

	return std::launder(static_cast<T *>(std::memmove(p, p, sizeof(T) * n)));
}
#endif
} // namespace exchange::core::util
