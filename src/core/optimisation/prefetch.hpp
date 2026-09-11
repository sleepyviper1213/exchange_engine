#pragma once

#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace exchange::core::optimisation {

/**
 * @brief A hint indicating how soon the prefetched line is wanted, and
 * therefore how close in it should be pulled.
 *
 * Maps to the intrinsics' locality_hint argument. The names say what the caller
 * knows rather than which cache level the hardware picks - a hint is not a
 * placement instruction, and no architecture here promises one.
 */
enum class locality_hint : std::uint8_t {
	/// @brief Streaming: touched once and not wanted again. Pulled in without
	///        evicting what the working set is using (`_MM_HINT_NTA`).
	none = 0,
	/// @brief Wanted again, but not immediately.
	low = 1,
	/// @brief Wanted again soon.
	moderate = 2,
	/// @brief Wanted immediately and repeatedly - pull it as close as the
	///        hardware allows (`_MM_HINT_T0`).
	high = 3,
};

/**
 * @brief Hint that @p address should be brought into cache for reading.
 *
 * @param address What to pull in. Any address is safe, including a null or an
 *        unmapped one: a prefetch never faults and never traps, which is what
 *        lets a walk prefetch one step past the end of a structure without
 *        checking first.
 * @param hint How soon it is wanted. @see locality_hint
 */
inline void prefetch_read(const void *address,
						  locality_hint hint = locality_hint::high) noexcept {
#ifdef _MSC_VER
	// _mm_prefetch takes the hint as a compile-time constant, so the switch is
	// what turns the runtime-typed parameter back into one. Every arm folds
	// away at a call site that passes a literal, which is all of them.
	switch (hint) {
	case locality_hint::none:
		_mm_prefetch(static_cast<const char *>(address), _MM_HINT_NTA);
		break;
	case locality_hint::low:
		_mm_prefetch(static_cast<const char *>(address), _MM_HINT_T2);
		break;
	case locality_hint::moderate:
		_mm_prefetch(static_cast<const char *>(address), _MM_HINT_T1);
		break;
	case locality_hint::high:
		_mm_prefetch(static_cast<const char *>(address), _MM_HINT_T0);
		break;
	}
#elif defined(__GNUC__) || defined(__clang__)
	// GCC and Clang take the locality_hint as a literal too, hence the same switch
	// rather than a cast of `hint` straight into the third argument.
	switch (hint) {
	case locality_hint::none: __builtin_prefetch(address, 0, 0); break;
	case locality_hint::low: __builtin_prefetch(address, 0, 1); break;
	case locality_hint::moderate: __builtin_prefetch(address, 0, 2); break;
	case locality_hint::high: __builtin_prefetch(address, 0, 3); break;
	}
#else
	// No intrinsic: the hint is dropped, which is exactly what a hint is
	// allowed to be. Discarding the argument keeps the call site warning-free.
	(void)address;
	(void)hint;
#endif
}

} // namespace exchange::core::optimisation
