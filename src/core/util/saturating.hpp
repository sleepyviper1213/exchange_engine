#pragma once

#include <concepts>
#include <version>
#ifdef __cpp_lib_saturation_arithmetic
#include <numeric>
#else

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#include <limits>
#include <type_traits>

#endif
namespace exchange::core::util {

template <std::integral T>
[[nodiscard]] constexpr T saturating_sub(T x, T y) noexcept {
#ifdef __cpp_lib_saturation_arithmetic
	return std::saturating_sub(x, y);
#else
	if (std::is_constant_evaluated()) {
		if constexpr (std::is_signed_v<T>) {
			using UT    = std::make_unsigned_t<T>;
			UT ux       = static_cast<UT>(x);
			UT uy       = static_cast<UT>(y);
			UT diff     = ux - uy;
			UT sign_bit = UT(1) << (sizeof(T) * 8 - 1);
			if ((ux ^ uy) & (diff ^ ux) & sign_bit) {
				return (ux & sign_bit) ? std::numeric_limits<T>::min()
									   : std::numeric_limits<T>::max();
			}
			return static_cast<T>(diff);
		} else {
			return (x < y) ? 0 : (x - y);
		}
	}
	// 2. Runtime optimized paths using compiler intrinsics
	else if constexpr (std::is_signed_v<T>) {
#if defined(__GNUC__) || defined(__clang__)
		T res;
		if (__builtin_sub_overflow(x, y, &res)) {
			return (x < 0) ? std::numeric_limits<T>::min()
						   : std::numeric_limits<T>::max();
		}
		return res;
#else
		// MSVC Signed Path: Lacking signed overflow intrinsics, use an
		// optimized branchless stream
		using UT    = std::make_unsigned_t<T>;
		UT ux       = static_cast<UT>(x);
		UT uy       = static_cast<UT>(y);
		UT diff     = ux - uy;
		UT sign_bit = UT(1) << (sizeof(T) * 8 - 1);
		UT overflow = (ux ^ uy) & (diff ^ ux) & sign_bit;
		if (overflow) {
			return (ux & sign_bit) ? std::numeric_limits<T>::min()
								   : std::numeric_limits<T>::max();
		}
		return static_cast<T>(diff);
#endif
	} else { // Unsigned runtime execution
#if defined(_MSC_VER) && !defined(__clang__)
		// MSVC Unsigned Intrinsic Implementation
		if constexpr (sizeof(T) == 8) {
			unsigned __int64 result;
			if (_subborrow_u64(0,
							   static_cast<unsigned __int64>(x),
							   static_cast<unsigned __int64>(y),
							   &result)) {
				return 0;
			}
			return static_cast<T>(result);
		} else {
			// Handles 8-bit, 16-bit, and 32-bit types via 32-bit hardware
			// register borrow
			unsigned int result;
			if (_subborrow_u32(0,
							   static_cast<unsigned int>(x),
							   static_cast<unsigned int>(y),
							   &result)) {
				return 0;
			}
			return static_cast<T>(result);
		}
#elif defined(__GNUC__) || defined(__clang__)
		T res;
		if (__builtin_sub_overflow(x, y, &res)) return 0;
		return res;
#else
		return (x < y) ? 0 : (x - y);
#endif
	}
#endif
}

} // namespace exchange::core::util
