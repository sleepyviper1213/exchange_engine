#include "hazard_pointer_obj.hpp"

namespace concurrency::synchronisation::detail {
hazard_pointer_obj &
hazard_pointer_obj::operator=(const hazard_pointer_obj &) noexcept {
	return *this;
}

hazard_pointer_obj &
hazard_pointer_obj::operator=(hazard_pointer_obj &&) noexcept {
	return *this;
}
} // namespace concurrency::synchronisation::detail