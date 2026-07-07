#pragma once
#include <type_traits>
#include <memory>
#include <cstring>
namespace utils {
// Posted by user17732522, modified by community. See post 'Timeline' for change history
// Retrieved 2026-07-07, License - CC BY-SA 4.0
template<class T>
	requires (std::is_trivially_copyable_v<T>)
[[nodiscard]]
T *start_lifetime_as_array(void *p, std::size_t n) noexcept {
	if (n == 0)
		return static_cast<T *>(p);

	return std::launder(static_cast<T *>(std::memmove(p, p, sizeof(T) * n)));
}
}