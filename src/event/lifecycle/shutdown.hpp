#pragma once
// Engine shut-down event.
//
// The record that closes a session - and the only one in this module whose
// value is mostly in its *absence*. A journal ending with a shutdown ended
// because somebody stopped the engine; a journal that simply stops ended
// because the process died, and its last entries may be torn. Nothing else in
// an append-only log can tell those two apart, because both look like "no more
// bytes".

#include "fwd.hpp"
#include "stop_reason.hpp"

#include <type_traits>

namespace exchange::engine::event::lifecycle {

/**
 * @brief The engine stopped; no further command in this session will be
 * applied.
 *
 * @par What the counts are for, and why these two
 * Both are numbers the engine already keeps, and together they are the cheapest
 * possible integrity check on a replay: re-apply the session's journal, and the
 * commands you applied must equal @c commands_applied and the events you saw
 * must equal @c events_published. A mismatch means the log and the run
 * disagree, which is exactly what a replay is supposed to detect and what a log
 * with no totals in it cannot. @c engine_partition::drain returns the first and
 * @c event_channel::published carries the second, so neither costs anything to
 * record.
 *
 * They are a checksum, not a sequence number. A per-command sequence - TODO.md
 * #7 - is what would let two *records* be ordered against each other; until
 * that exists, position in the append-only log is the order, and these totals
 * are how a reader checks it did not lose any.
 *
 * @par Why the timestamp is wall clock
 * @copydoc startup
 *
 * @code
 * const lifecycle::shutdown closed{.session          = run_id,
 *                                  .timestamp        =
 * clock.core::chrono::wall_now(), .reason           = stop_reason::CLEAN,
 *                                  .commands_applied = applied,
 *                                  .events_published = channel.published()};
 * @endcode
 */
struct shutdown {
	session_id_t session = 0;           ///< the session this record closes
	wall_time timestamp;                ///< when it happened, wall clock
	stop_reason reason =
		stop_reason::CLEAN;              ///< how much of the session to believe
	std::uint64_t commands_applied = 0; ///< commands the session executed
	std::uint64_t events_published = 0; ///< trades and outcomes it published

	bool operator==(const shutdown &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<shutdown>,
			  "a lifecycle record must stay trivially copyable so a journal "
			  "append is a raw write");

} // namespace exchange::engine::event::lifecycle
