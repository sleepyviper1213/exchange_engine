#pragma once
// Where the gate gets "now" from, and why it is a template parameter.

#include "fwd.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>

namespace exchange::risk {

/**
 * @brief A monotonic source of nanoseconds the gate can be built on.
 *
 * @par Why the clock is injected rather than called
 * Two reasons, and the second is the important one.
 *
 * The obvious one is testability: a rate limit and a breach window are both
 * defined in terms of elapsed nanoseconds, and a test that has to *sleep*
 * across a window boundary to check them is slow and flaky in the same breath.
 * With the clock injected the boundary is a variable, so a test can sit one
 * nanosecond either side of it and get a deterministic answer.
 *
 * The one that matters in production is that a process which already knows the
 * time should not be made to ask again. An engine receiving
 * hardware-timestamped packets has a better "now" than @c steady_clock::now()
 * and has already paid for it; a replay driving recorded traffic needs the
 * *recorded* time or its rate limits fire in the wrong places. Both are a clock
 * type, and neither is reachable if the call is baked in.
 */
template <class C>
concept nanosecond_clock = requires(const C &clock) {
	{ clock.now_ns() } -> std::same_as<std::uint64_t>;
};

/**
 * @brief The default clock: @c std::chrono::steady_clock in nanoseconds.
 *
 * Steady rather than system, because every consumer of this value is measuring
 * an *interval* - how far into a window, how long since the last breach - and a
 * clock that can be stepped by NTP turns an interval into a negative number and
 * a rate limiter into a gate that is open all day.
 *
 * @note Called once per batch, never once per command. A @c now() is tens of
 *       nanoseconds on a good day, which is several times the whole per-command
 *       budget; amortising it over a batch is what keeps the check honest about
 *       its cost.
 */
struct steady_nanos {
	[[nodiscard]] std::uint64_t now_ns() const noexcept {
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count());
	}
};

static_assert(nanosecond_clock<steady_nanos>);

} // namespace exchange::risk
