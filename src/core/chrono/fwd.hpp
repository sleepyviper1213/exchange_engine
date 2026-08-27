#pragma once

// Forward declarations for the time module, and the one place that says what
// the project's clocks are called and which is right for what.
//
// @par Why they are one module
// There are three questions a program asks about time and they are not
// interchangeable, so this project answers each with a *distinct type* rather
// than with three `std::uint64_t`s and a comment:
//
//   * "how long between these two things" - `monotonic_time`, from whichever
//     monotonic clock a deployment injected. Never steppable, so an interval is
//     never negative. Every rate window, breach window and schedule.
//   * "when in the real world" - `wall_time`, from `wall_now`. Has an epoch
//     that survives a restart and lines up with an operator's incident
//     timeline, an exchange's session schedule, another service's log. Session
//     boundaries, and nothing that measures an interval.
//   * "when did this process take delivery of these bytes" - `ingress_time`,
//     from `ingress_clock`. Monotonic like the first, but a *different* clock
//     with a different epoch, so subtracting it from a venue's own stamp is a
//     mistake the compiler catches instead of a number nobody can decompose.
//
namespace exchange::core::chrono {

/// @brief Tag for engine-monotonic time.
struct monotonic_clock;

/// @brief The default monotonic clock: `steady_clock` in nanoseconds.
struct steady_nanos;

/// @brief Tag for local delivery stamps. 
struct ingress_clock;

/// @brief Replay time, driven by a capture's venue stamps. 
class feed_clock;

/// @brief A non-owning reader of a @c feed_clock.
class clock_view;

// `nanosecond_clock` is deliberately absent: a concept cannot be forward
// declared, and a header that wants to constrain on it wants clock.hpp anyway.

} // namespace exchange::core::chrono
