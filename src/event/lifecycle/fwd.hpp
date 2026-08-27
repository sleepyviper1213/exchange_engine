#pragma once
// Forward declarations and the shared vocabulary for the lifecycle event
// submodule.

#include <chrono>
#include <cstdint>

namespace exchange::engine::event::lifecycle {

/**
 * @brief Identifies one run of the engine - a *session*.
 *
 * The name for something the tree already depends on everywhere and never
 * spelled. @c order_manager::clear, @c position_book::forget,
 * @c working_ledger::clear, @c rate_limiter::reset and @c counter::reset each
 * document themselves as "a session boundary", and @c order_id_t's contract is
 * that a client order id is unique *within* a session. That contract is what
 * makes the boundary load-bearing rather than administrative: replaying two
 * sessions' commands into one book without one collides their ids and every
 * collision comes back as DUPLICATE_ORDER_ID.
 *
 * So a session is the scope order ids are unique in, and this is its name.
 * Assigned by whatever starts the engine - a wall-clock stamp, a run counter, a
 * deployment id - and required only to differ from the previous one, since the
 * only question ever asked of it is "is this the same session as that
 * record's".
 *
 * @note 64 bits, and monotonic if the assigner can manage it: a replay reading
 *       a log backwards can then stop at the first session older than the one
 *       it wants without decoding what follows.
 */
using session_id_t = std::uint64_t;

/**
 * @brief When a lifecycle record happened: a wall-clock instant, in
 * nanoseconds.
 *
 * A @c time_point and not a count of nanoseconds, because the clock is part of
 * the type. Every record here is stamped from the system clock and every
 * *interval* the risk gate measures is stamped from a steady one, and while
 * both were spelled @c std::uint64_t the two were freely interchangeable - the
 * headers could argue about the difference and nothing could enforce it.
 * Feeding a steady reading into a session boundary compiled, and produced a
 * timestamp measured from process start: correlated with nothing outside this
 * process, and running backwards across a restart. It is now a type error.
 *
 * @note Still 8 bytes and still trivially copyable, so a record carrying one is
 *       unchanged on disk and the static_asserts below hold.
 */
using wall_time = std::chrono::sys_time<std::chrono::nanoseconds>;

/**
 * @brief @p when as nanoseconds since the Unix epoch - the form a machine
 * reads.
 *
 * The counterpart to @c wall_time being a @c time_point, and the reason that
 * costs nothing. A record's stamp is a typed instant everywhere it is stored,
 * compared or passed; it becomes a bare integer at exactly two kinds of place,
 * a log field and a wire field, and both want the same thing for the same
 * reason - an epoch count is unambiguous across every consumer's timezone and
 * locale, where a rendered date is not.
 *
 * So this is that conversion, named, rather than @c time_since_epoch().count()
 * spelled at each of them. What it buys is not brevity: an unwrapped count is
 * assignable to any integer in scope, so every explicit unwrap was a place the
 * distinction @c wall_time exists to draw could be silently dropped. Now the
 * unwrapping happens in one function that says why.
 *
 * @note Signed, matching @c nanoseconds::rep, and not widened to
 *       @c session_id_t's unsigned - an instant before 1970 is representable
 * and should stay negative rather than become an enormous positive. @see
 *       session_of for the one caller that does want it unsigned.
 */
[[nodiscard]] constexpr std::int64_t epoch_nanos(wall_time when) noexcept {
	return when.time_since_epoch().count();
}

/**
 * @brief A session id derived from the instant the session opened.
 *
 * What every driver in @c app/commands already did by hand, spelled identically
 * three times. It satisfies both halves of @c session_id_t's contract by
 * construction: distinct from the previous run's, because no two runs of a
 * process open in the same nanosecond, and monotonic, because the wall clock is
 * - which is the property that lets a replay reading a log backwards stop at
 * the first session older than the one it wants.
 *
 * @param opened The wall-clock instant the session began, from @c
 * core::chrono::wall_now.
 * @return The id, as an unsigned count of nanoseconds.
 *
 * @note Deriving rather than counting is a deployment's choice, and this is the
 *       one this tree makes; @c session_id_t itself requires only that
 *       consecutive ids differ, so a run counter or a deployment id remains a
 *       legitimate alternative. It lives here rather than in @c app/ so that
 * the vocabulary defining the contract also offers the way of satisfying it.
 */
[[nodiscard]] constexpr session_id_t session_of(wall_time opened) noexcept {
	return static_cast<session_id_t>(epoch_nanos(opened));
}

struct startup;
struct shutdown;
struct recovery;

} // namespace exchange::engine::event::lifecycle
