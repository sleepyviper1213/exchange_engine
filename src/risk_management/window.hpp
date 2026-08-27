#pragma once
// Bucketing a monotonic reading into fixed-width windows, and the one place
// this module takes the scalar back out of a time_point.

#include "core/chrono/clock.hpp"

#include <cstdint>

namespace exchange::risk {

/**
 * @brief Which fixed-width window @p when falls in, for windows @c 2^log2_ns
 *        wide.
 *
 * The one operation in this module that legitimately needs the scalar out of a
 * @c core::chrono::monotonic_time, and therefore the only place that takes it
 * out. Both
 * @c rate_limiter and @c circuit_breaker bucket time this way - a shift rather
 * than a division, which is the whole reason their windows are stated as a
 * power of two - and both used to spell
 * @c static_cast<std::uint64_t>(now.time_since_epoch().count()) >> shift_ at
 * every use. That is three separable decisions (unwrap the time_point, widen to
 * unsigned, bucket) written as one expression, four times over, and none of
 * them says which of the three is the point.
 *
 * @param when The instant to bucket. Monotonic, so non-negative: the widening
 *        to unsigned cannot wrap a reading this clock produced, and comparing
 *        two windows is then a plain integer compare with no sign to reason
 *        about.
 * @param log2_ns Window width as a power-of-two nanosecond count - the
 *        @c window_log2_ns both hooks are configured with.
 * @return An opaque window index. Only equality between two of them is
 *         meaningful; the value itself names no instant.
 *
 * @note Not a @c std::chrono operation on purpose. @c floor<D> would express
 * the same bucketing in the type system and return a @c time_point, but it
 *       divides by an arbitrary period where this shifts, and the shift is a
 *       measured property of both call sites rather than an implementation
 *       detail. @see rate_limiter, circuit_breaker
 */
[[nodiscard]] constexpr std::uint64_t
window_of(core::chrono::monotonic_time when, unsigned log2_ns) noexcept {
	return static_cast<std::uint64_t>(when.time_since_epoch().count()) >>
		   log2_ns;
}

} // namespace exchange::risk
