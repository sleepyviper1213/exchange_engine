#include "core/simd/target.hpp"

#include <hwy/highway.h>
#include <hwy/per_target.h>
#include <hwy/targets.h>

#include <cstddef>
#include <string_view>

namespace exchange::core::simd {

std::string_view active_target() noexcept {
	return hwy::TargetName(hwy::DispatchedTarget());
}

std::size_t register_bytes() noexcept { return hwy::VectorBytes(); }

} // namespace exchange::core::simd
