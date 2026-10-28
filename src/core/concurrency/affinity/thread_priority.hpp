#pragma once
// The scheduling tiers a thread may ask for, and nothing else.
//
// Split from affinity.hpp so a header that only needs to *name* a priority - a
// role table, a reservation, a placement log - does not also pull in the
// syscall layer's declarations.

#include "core/util/enum_format.hpp"

#include <cstdint>

namespace exchange::core::concurrency::affinity {

// Spelled from the root: topology.hpp declares a struct `core` in this
// namespace, and a relative `core::util` resolves into it whenever that header
// happens to be included first.
using exchange::core::util::format_as;

/// Scheduling priority for a thread. Best-effort and relative - the exact OS
/// policy differs, but a higher tier always preempts a lower one on the same
/// core. Raising priority may need privileges (an elevated process on Windows;
/// @c CAP_SYS_NICE / a real-time-capable limit on Linux); denial costs
/// scheduling determinism, never correctness.
///
/// Prints as its enumerator name, which is what puts it in a placement log.
enum class thread_priority : std::uint8_t {
	normal,   ///< OS default
	high,     ///< above background work - matching engine, producer, consumer
	realtime, ///< highest achievable; time-critical, usually needs privileges
};

} // namespace exchange::core::concurrency::affinity
