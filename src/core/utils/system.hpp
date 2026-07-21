#pragma once

// Hard thread affinity is a platform service. GetCurrentThread() returns a
// Win32 pseudo-handle valid for the caller regardless of whether std::thread is
// backed by winpthreads, so each thread pins itself from inside and we never
// touch std::thread::native_handle() (a pthread_t under MinGW, not a HANDLE).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace core::utils {
[[nodiscard, gnu::always_inline]] inline bool
pin_current_thread_to_core(unsigned cpu_core) noexcept {
#if defined(_WIN32)
	const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu_core;
	return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
	cpu_core_set_t set;
	cpu_core_ZERO(&set);
	cpu_core_SET(cpu_core, &set);
	return sched_setaffinity(0, sizeof(set), &set) == 0;
#elif defined(__APPLE__)
	// macOS doesn't support hard-affinity
	static_cast<void>(cpu_core);
	return false;
#endif
}
} // namespace core::utils