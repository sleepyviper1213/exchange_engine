#pragma once
// Real time: the clock that stamps a point somebody outside this process can
// line up with. @see fwd.hpp for how this relates to monotonic and ingress time.

#include <chrono>

namespace exchange::core::chrono {

/**
 * @brief A point in real time, to nanosecond resolution.
 *
 * @c std::chrono::sys_time by definition rather than by convention, which is
 * what makes it interchangeable with @c event::lifecycle::wall_time without
 * either naming the other: both *are* the same standard type, so a reading from
 * @c wall_now is the only thing a lifecycle record's timestamp will accept, and
 * a monotonic reading no longer compiles there. The two used to both be
 * @c std::uint64_t and the difference lived in comments.
 */
using wall_time = std::chrono::sys_time<std::chrono::nanoseconds>;

/**
 * @brief Now, on the wall clock.
 *
 * @par Why this is not @c steady_nanos, when the two look interchangeable
 * Everything a rate window or a breach window measures is an *interval*, so it
 * needs a clock NTP cannot step backwards. Nothing here is an interval. A
 * session boundary is a point in real time whose entire job is to line up with
 * something outside this process: an exchange's session schedule, an operator's
 * incident timeline, another service's log. A steady clock's epoch is arbitrary
 * and does not survive a restart, which makes it precisely useless for that.
 *
 * @note @c event/lifecycle ships no default clock on purpose - its records take
 *       a timestamp from the caller because which clock is right is a
 *       deployment's decision rather than the vocabulary's. This is that
 *       decision available in one place, not imposed on the vocabulary; it used
 *       to be made three times, identically, in demo.cpp, recover.cpp and
 *       serve.cpp, which is three places for one of them to drift.
 */
[[nodiscard]] inline wall_time wall_now() noexcept {
	return std::chrono::time_point_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now());
}

} // namespace exchange::core::chrono
