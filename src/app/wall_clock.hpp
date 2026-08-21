#pragma once
// The process's answer to "which clock stamps a session boundary".
//
// `event/lifecycle` ships no default clock on purpose: its records take a
// timestamp from the caller because which clock is right is a deployment's
// decision rather than the vocabulary's. This is that decision, made once for
// this executable - it used to be made three times, identically, in demo.cpp,
// recover.cpp and serve.cpp, which is three places for one of them to drift.
//
// It goes the opposite way from `risk::steady_nanos`, and the two look
// interchangeable enough to be worth stating. Everything the risk gate times is
// an *interval* - how far into a rate window, how long since a breach - so it
// needs a clock NTP cannot step backwards. Nothing here is an interval. A
// session boundary is a point in real time whose entire job is to line up with
// something outside this process: an exchange's session schedule, an operator's
// incident timeline, another service's log. A steady clock's epoch is arbitrary
// and does not survive a restart, which makes it precisely useless for that.

#include <chrono>
#include <cstdint>

namespace exchange::app {

/**
 * @brief Now, on the wall clock, to nanosecond resolution.
 *
 * Returns a @c time_point rather than a count, which is the same type as
 * @c event::lifecycle::wall_time - deliberately, and by definition rather than
 * by convention, so a reading from here is the only thing a lifecycle record's
 * timestamp will accept. A steady reading is a different type and no longer
 * compiles there, which is the whole point: the two were both
 * @c std::uint64_t and the difference lived in comments.
 */
[[nodiscard]] inline std::chrono::sys_time<std::chrono::nanoseconds>
wall_now() noexcept {
	return std::chrono::time_point_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now());
}

} // namespace exchange::app
