#pragma once

#include "fwd.hpp"

#include <cstdint>
#include <thread>

// Hard thread-affinity — the syscall layer. Each thread pins *itself* from the
// inside: GetCurrentThread()/pthread_self() name the caller regardless of how
// std::thread is backed, so we never touch std::thread::native_handle() (a
// pthread_t under MinGW, not a Win32 HANDLE). All operations are best-effort:
// a false return (unsupported platform, out-of-range core, denied permission)
// costs scheduling determinism, never correctness.
//
// The platform headers are pulled in here rather than in a widely-included
// header so <windows.h> only reaches the few translation units that pin.
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#elifdef __linux__
#include <numeric>
#include <pthread.h>
#include <sched.h>
#endif

namespace concurrency::affinity {

/// Number of logical CPUs the process may run on. Falls back to 1 when the
/// runtime cannot report it (hardware_concurrency() is allowed to return 0).
[[nodiscard]] inline unsigned logical_cpu_count() noexcept {
	const unsigned n = std::thread::hardware_concurrency();
	return n == 0U ? 1U : n;
}

/// Restrict the CALLING thread to the logical CPUs whose bit is set in @p mask
/// (bit i == CoreId i). Limited to the first 64 CPUs — sufficient here and the
/// portable common denominator across Win32 and glibc.
/// @return true on success; false on failure or an unsupported platform.
[[nodiscard]] inline bool
set_this_thread_affinity(std::uint64_t mask) noexcept {
	if (mask == 0U) return false;
#ifdef _WIN32
	return SetThreadAffinityMask(GetCurrentThread(),
								 static_cast<DWORD_PTR>(mask)) != 0;
#elifdef __linux__
	cpu_set_t set;
	CPU_ZERO(&set);
	for (unsigned cpu = 0; cpu < 64U; ++cpu)
		if ((mask >> cpu) & 1U) CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
	// macOS exposes no hard-affinity primitive; treat as unsupported.
	return false;
#endif
}

/// Pin the CALLING thread to a single logical CPU.
/// @return true on success; false for @c kNoCore, a core past the 64-bit mask,
///         a failed syscall, or an unsupported platform.
[[nodiscard]] inline bool pin_this_thread(CoreId core) noexcept {
	if (core == kNoCore || core >= 64U) return false;
	return set_this_thread_affinity(std::uint64_t{1} << core);
}

/// Scheduling priority for a thread. Best-effort and relative — the exact OS
/// policy differs, but a higher tier always preempts a lower one on the same
/// core. Raising priority may need privileges (an elevated process on Windows;
/// @c CAP_SYS_NICE / a real-time-capable limit on Linux); denial costs
/// scheduling determinism, never correctness.
enum class ThreadPriority {
	Normal,   ///< OS default.
	High,     ///< Above background work — matching engine, producer, consumer.
	Realtime, ///< Highest achievable; time-critical. Usually needs privileges.
};

/// Set the CALLING thread's scheduling priority. Call from inside that thread,
/// same self-applied contract as pin_this_thread.
/// @return true on success; false on failure (insufficient privileges,
///         unsupported platform).
[[nodiscard]] inline bool
set_this_thread_priority(ThreadPriority priority) noexcept {
#if defined(_WIN32)
	int level = THREAD_PRIORITY_NORMAL;
	switch (priority) {
		using enum ThreadPriority;
	case Normal: level = THREAD_PRIORITY_NORMAL; break;
	case High: level = THREAD_PRIORITY_HIGHEST; break;
	case Realtime: level = THREAD_PRIORITY_TIME_CRITICAL; break;
	}
	return SetThreadPriority(GetCurrentThread(), level) != 0;
#elif defined(__linux__)
	// Normal rides the default fair scheduler (SCHED_OTHER, nice 0); the hot
	// tiers use real-time SCHED_FIFO, whose priorities need CAP_SYS_NICE — a
	// denied call just returns false.
	int policy = SCHED_OTHER;
	sched_param param{};
	if (priority != ThreadPriority::Normal) {
		policy       = SCHED_FIFO;
		const int lo = sched_get_priority_min(policy);
		const int hi = sched_get_priority_max(policy);
		param.sched_priority =
			priority == ThreadPriority::Realtime ? hi : std::midpoint(lo, hi);
	}
	return pthread_setschedparam(pthread_self(), policy, &param) == 0;
#else
	static_cast<void>(priority);
	return false;
#endif
}

} // namespace concurrency::affinity
