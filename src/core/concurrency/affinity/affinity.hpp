#pragma once

#include "core/util/enum_string.hpp"
#include "core_export.hpp" // CORE_EXPORT (generated)
#include "fwd.hpp"

#include <cstdint>


// Hard thread-affinity — the syscall layer. Each thread pins *itself* from the
// inside: GetCurrentThread()/pthread_self() name the caller regardless of how
// std::thread is backed, so we never touch std::thread::native_handle() (a
// pthread_t under MinGW, not a Win32 HANDLE). All operations are best-effort:
// a false return (unsupported platform, out-of-range core, denied permission)
// costs scheduling determinism, never correctness.
//
// The definitions live in affinity.cpp, which is the only translation unit in
// the project that includes <windows.h> (or <pthread.h>/<sched.h>). Pinning is
// a startup activity — none of this is on a hot path — so the cross-module call
// costs nothing worth measuring, and in exchange this header stays four
// declarations no matter how many places pin.
namespace exchange::core::concurrency::affinity {

/// Number of logical CPUs the process may run on. Falls back to 1 when the
/// runtime cannot report it (hardware_concurrency() is allowed to return 0).
[[nodiscard]] CORE_EXPORT unsigned logical_cpu_count() noexcept;

/// Restrict the CALLING thread to the logical CPUs whose bit is set in @p mask
/// (bit i == core_id i). Limited to the first 64 CPUs — sufficient here and the
/// portable common denominator across Win32 and glibc.
/// @return true on success; false on failure or an unsupported platform.
[[nodiscard]] CORE_EXPORT bool
set_this_thread_affinity(std::uint64_t mask) noexcept;

/// Pin the CALLING thread to a single logical CPU.
/// @return true on success; false for @c kNoCore, a core past the 64-bit mask,
///         a failed syscall, or an unsupported platform.
[[nodiscard]] CORE_EXPORT bool pin_this_thread(core_id core) noexcept;

/// Scheduling priority for a thread. Best-effort and relative — the exact OS
/// policy differs, but a higher tier always preempts a lower one on the same
/// core. Raising priority may need privileges (an elevated process on Windows;
/// @c CAP_SYS_NICE / a real-time-capable limit on Linux); denial costs
/// scheduling determinism, never correctness.
#define THREAD_PRIORITY_LIST(X)                                                \
	X(normal, "OS default")                                                    \
	X(high, "above background work — matching engine, producer, consumer")     \
	X(realtime, "highest achievable; time-critical, usually needs privileges")

enum class thread_priority : std::uint8_t {
	EXCHANGE_ENUM_VALUES(THREAD_PRIORITY_LIST)
};

/// @brief The enumerator name of a @c thread_priority, e.g. @c "realtime".
///        Also makes it printable, which is what puts it in a placement log.
EXCHANGE_ENUM_NAME(thread_priority, to_string, THREAD_PRIORITY_LIST)

/// Set the CALLING thread's scheduling priority. Call from inside that thread,
/// same self-applied contract as pin_this_thread.
/// @return true on success; false on failure (insufficient privileges,
///         unsupported platform).
[[nodiscard]] CORE_EXPORT bool
set_this_thread_priority(thread_priority priority) noexcept;

} // namespace exchange::core::concurrency::affinity
