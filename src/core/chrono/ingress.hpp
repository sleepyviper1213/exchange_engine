#pragma once
// Delivery stamps: when this process took receipt of some bytes.

#include "fwd.hpp"

#include <chrono>

namespace exchange::core::chrono {

/**
 * @brief The local monotonic clock an ingress stamp is read from.
 *
 * @par Why this is not a venue timestamp, and cannot be mixed with one
 * Because the two answer different questions and the difference is invisible
 * once both are spelled @c nanoseconds. A venue's event time is the *venue's*
 * clock: subtracting it from a local reading measures the two machines' clock
 * offset plus the one-way delay plus whatever queueing happened, all summed
 * into one number nobody can decompose. An ingress stamp is this process's own
 * clock, so a difference between it and a later reading of the same clock is an
 * elapsed interval and nothing else - which is what a reaction time is.
 *
 * Making it a distinct @c time_point rather than another duration is what stops
 * the first mistake being made silently: a venue time no longer converts to an
 * ingress stamp, or an ingress stamp to a venue time, so the two cannot be
 * subtracted from one another at all.
 *
 * @par Why this one has a now() where monotonic_clock deliberately does not
 * That tag leaves @c now() to a template parameter because *which* monotonic
 * clock a gate reads is a deployment's choice - a replay wants recorded time, a
 * NIC-timestamped feed wants the hardware's. There is no such choice here. An
 * ingress stamp records when this process took delivery of some bytes, which
 * only this process's own clock can say, and it is read at the transport edge
 * where there is nothing above to inject anything. One clock, one caller, so
 * the clock is named rather than parameterised.
 *
 * @note @c steady_clock::now() costs tens of nanoseconds (see
 *       docs/performance.md - ~32 ns p50, ~73 ns p99 on Windows), which a
 *       100 ms diff stream does not notice and a 100 ns-per-message binary feed
 *       could not afford. A venue at that rate wants the NIC's own stamp
 *       carried up from @c transport::packet_view, not this.
 */
struct ingress_clock {
	using rep        = std::int64_t;
	using period     = std::nano;
	using duration   = std::chrono::duration<rep, period>;
	using time_point = std::chrono::time_point<ingress_clock, duration>;

	// NOLINTNEXTLINE(readability-identifier-naming)
	static constexpr bool is_steady = true;

	/// @brief Now, on the receiving process's monotonic clock.
	///
	/// The epoch is unrelated to any other clock's and is meaningless on its
	/// own - only differences between two readings of this clock mean anything.
	[[nodiscard]] static time_point now() noexcept {
		return time_point{std::chrono::duration_cast<duration>(
			std::chrono::steady_clock::now().time_since_epoch())};
	}
};

/// @brief When this process took delivery of a message's bytes.
///        @see ingress_clock
using ingress_time = ingress_clock::time_point;

/// @brief Whether @p stamp was actually taken.
///
/// A default-constructed stamp is the "nobody stamped this" value, and it has
/// to be distinguishable: a scripted test event, a replayed capture and a
/// snapshot decoded offline all legitimately carry none, and treating the
/// clock's own epoch as an arrival time would report the process's uptime as a
/// reaction time.
[[nodiscard]] constexpr bool has_ingress(ingress_time stamp) noexcept {
	return stamp.time_since_epoch() != ingress_clock::duration::zero();
}

} // namespace exchange::core::chrono
