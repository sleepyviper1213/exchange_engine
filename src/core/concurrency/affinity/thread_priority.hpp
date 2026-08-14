#pragma once
// The scheduling tiers a thread may ask for, and nothing else.
//
// Split from affinity.hpp so a header that only needs to *name* a priority — a
// role table, a reservation, a placement log — does not also pull in the
// syscall layer's declarations. The enum and the X-macro list that generates
// its names are one unit and travel together.

#include "core/util/enum_string.hpp"

#include <cstdint>

namespace exchange::core::concurrency::affinity {

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

#undef THREAD_PRIORITY_LIST
} // namespace exchange::core::concurrency::affinity
