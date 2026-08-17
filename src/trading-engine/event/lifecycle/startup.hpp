#pragma once
// Engine start-up event.
//
// The record that opens a session, and the one that tells a reader of the log
// which session everything after it belongs to. Without it a journal is a flat
// sequence of commands whose order ids may or may not mean the same thing from
// one entry to the next, because a client order id is only unique inside a
// session. @see lifecycle/fwd.hpp on what a session is and why.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>
#include <type_traits>

namespace exchange::engine::event::lifecycle {

#define START_MODE_LIST(X)                                                     \
	X(COLD, "empty books; the previous session's state was discarded")         \
	X(RECOVERED, "state rebuilt from a snapshot and journal; ids continue")

/**
 * @brief Whether the session inherited the previous one's state.
 *
 * The single most consequential thing a start-up record says, because it decides
 * what the order ids that follow mean. After a COLD start the id space is empty
 * and an id seen in the previous session may legitimately reappear naming a
 * different order; after a RECOVERED start the same id still names the same
 * resting order, and reusing it is the duplicate the book rejects.
 *
 * A reader that ignores this distinction and replays two COLD sessions into one
 * book gets DUPLICATE_ORDER_ID on every id that repeats — which is the failure
 * behaving correctly, but it is a failure that this one byte prevents.
 */
enum class StartMode : std::uint8_t { EXCHANGE_ENUM_VALUES(START_MODE_LIST) };

EXCHANGE_ENUM_NAME(StartMode, to_string, START_MODE_LIST)

#undef START_MODE_LIST

/**
 * @brief The engine reached running state and will begin accepting commands.
 *
 * @par Why the timestamp is wall clock and not the steady clock
 * The exact inverse of @c risk::steady_nanos' argument, and worth stating
 * because the two look interchangeable. Everything the risk gate times is an
 * *interval* — how far into a rate window, how long since a breach — so it must
 * use a clock NTP cannot step backwards. Nothing here is an interval. A session
 * boundary is a point in real time whose entire job is to be correlated with
 * something outside this process: an exchange's session schedule, an operator's
 * incident timeline, another service's log. A steady clock's epoch is arbitrary
 * and does not survive a restart, which makes it precisely useless for that.
 *
 * @par Why the caller supplies it
 * Same reason the gate takes its clock rather than calling one: a process that
 * already knows the time should not be made to ask again, and a replay driving
 * recorded traffic needs the *recorded* boundary, not the moment it re-read it.
 * There is no default clock in this header for that reason — @c event/ is
 * vocabulary, and "which clock" is a deployment's decision.
 *
 * @par Why there are no named factories
 * @c order_outcome has them because it *derives* fields from an @c order_state;
 * every field here is supplied whole by the caller, so a factory would only
 * rename the arguments. A designated initialiser names them at the call site
 * instead, which is the same protection without the indirection:
 *
 * @code
 * const lifecycle::startup opened{.session      = run_id,
 *                                 .timestamp_ns = clock.wall_ns(),
 *                                 .mode         = lifecycle::StartMode::COLD};
 * @endcode
 *
 * @note Trivially copyable, like @c command and @c engine_event, so a journal
 *       append is a raw write of the bytes rather than a serialisation step.
 */
struct startup {
	session_id_t session       = 0; ///< the session this record opens
	std::uint64_t timestamp_ns = 0; ///< wall clock, ns since the UNIX epoch
	StartMode mode = StartMode::COLD; ///< what became of the last session's state

	bool operator==(const startup &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<startup>,
			  "a lifecycle record must stay trivially copyable so a journal "
			  "append is a raw write");

} // namespace exchange::engine::event::lifecycle
