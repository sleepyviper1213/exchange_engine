#include "affinity.hpp"

#include <cstdint>
#include <thread>

// The platform headers are confined to this translation unit. Keeping them out
// of affinity.hpp is the whole reason these definitions are here: <windows.h>
// drags in a large surface (and needs the LEAN_AND_MEAN/NOMINMAX guards below
// to stay tolerable), and a header that pulled it in would push that cost onto
// every consumer of the affinity layer.
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

namespace exchange::core::concurrency::affinity {

unsigned logical_cpu_count() noexcept {
	const unsigned n = std::thread::hardware_concurrency();
	return n == 0U ? 1U : n;
}

bool set_this_thread_affinity(std::uint64_t mask) noexcept {
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

bool pin_this_thread(CoreId core) noexcept {
	if (core == kNoCore || core >= 64U) return false;
	return set_this_thread_affinity(std::uint64_t{1} << core);
}

bool set_this_thread_priority(ThreadPriority priority) noexcept {
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

} // namespace exchange::core::concurrency::affinity
