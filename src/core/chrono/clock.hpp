#pragma once
// Engine-monotonic time: the tag, the concept a clock satisfies, and the
// default clock. @see fwd.hpp for how this relates to wall and ingress time.

#include "fwd.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>

namespace exchange::core::chrono {

/**
 * @brief The engine's monotonic time base. A clock *tag*, not a clock.
 *
 * It exists to give the readings a type of their own, and the reason it is not
 * @c std::chrono::steady_clock is that not every clock here is one. A rate
 * window needs time that cannot run backwards; where that comes from is a
 * deployment's choice, and one of the choices already in the tree is
 * @c feed_clock, which advances on a capture's *venue event times*. Those are
 * wall-clock instants from another machine, made monotonic by being refused
 * when they regress. Requiring @c steady_clock::time_point would force that
 * clock to lie about what it returns.
 *
 * So: one tag every monotonic source here can honestly produce, and - the point
 * of the exercise - distinct from @c wall_time. A wall-clock reading is not
 * assignable to anything that wants monotonic time, which is the mistake this
 * header and @c chrono/wall.hpp could previously only argue about in prose,
 * because both were spelled @c std::uint64_t.
 *
 * @note No @c now(). A tag needs only the type members that give @c time_point
 *       its identity; asking *which* monotonic clock is the template
 *       parameter's job, which is the whole design below.
 */
struct monotonic_clock {
	using rep        = std::int64_t;
	using period     = std::nano;
	using duration   = std::chrono::duration<rep, period>;
	using time_point = std::chrono::time_point<monotonic_clock, duration>;
	// The std Clock requirements mandate this spelling, so the project's
	// UPPER_CASE rule for constants cannot apply to it.
	// NOLINTNEXTLINE(readability-identifier-naming)
	static constexpr bool is_steady = true;
};

/// @brief A reading from whichever monotonic clock a deployment injected.
///        Subtracting two gives a @c std::chrono::nanoseconds interval, which
///        is what every rule measuring elapsed time actually wants.
using monotonic_time = monotonic_clock::time_point;

/**
 * @brief A monotonic source of nanoseconds a component can be built on.
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
 * type, and neither is reachable if the call is baked in. @see feed_clock
 */
template <class C>
concept nanosecond_clock = requires(const C &clock) {
	{ clock.now() } -> std::same_as<monotonic_time>;
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
	/// @brief Now, as engine-monotonic time.
	///
	/// The one place a @c steady_clock reading is adopted as @c monotonic_time.
	/// The epochs are unrelated and neither is meaningful on its own - only
	/// differences are - so re-tagging the duration is the whole conversion.
	[[nodiscard]] static monotonic_time now() noexcept {
		return monotonic_time{
			std::chrono::duration_cast<monotonic_clock::duration>(
				std::chrono::steady_clock::now().time_since_epoch())};
	}

	/// @brief Deprecated: the raw reading. @see now
	/// @deprecated Being removed as callers move to @c now(); see fwd.hpp's
	///             note on why an untyped nanosecond count was a hazard.
	[[nodiscard]] static std::uint64_t now_ns() noexcept {
		return static_cast<std::uint64_t>(now().time_since_epoch().count());
	}
};

static_assert(nanosecond_clock<steady_nanos>);

} // namespace exchange::core::chrono
