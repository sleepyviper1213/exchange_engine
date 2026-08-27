#pragma once

#include <cstddef>
#include <string>

// Configuration vocabulary - data only, no behaviour.
//
// core/config/ rather than core/logging/ because configuration is its own
// concern, not a part of the thing being configured. Logging happens to be the
// only section today; an engine or transport section would have no natural home
// under core/logging/, and putting one there would mean every module owning a
// private notion of "settings" that the loader has to know about separately.
// One directory, one struct per section, siblings as they arrive.
//
// Deliberately free of <spdlog/spdlog.h>: a translation unit that merely
// *describes* how the process should log (main, a config loader, a test
// fixture) has no reason to compile the whole logging library to do it. Only
// code that actually emits messages needs that, and it gets it from
// core/logging.hpp.
namespace exchange::core::logging {

/// @brief How the process should log. The @c [logging] INI section, as a type.
struct settings {
	/// spdlog level name: trace | debug | info | warn | error | critical | off.
	/// An unrecognised name falls back to info with a warning rather than an
	/// error
	std::string level = "info";

	/// Logs always go to stderr; when this is non-empty they also go here.
	/// Empty disables the file sink.
	std::string log_file = "exchange_tool.log";

	std::string logger_name = "exchange";

	/**
	 * @brief Emit one JSON object per line instead of the human-readable
	 *        pattern, for a log a machine ingests rather than a terminal
	 *        someone reads.
	 *
	 * Costs nothing extra to gate: this only chooses which
	 * @c spdlog::formatter the logger installs at @c init time, so it is a
	 * one-time setup decision, not a per-call branch - a @c spdlog::debug
	 * below @c SPDLOG_ACTIVE_LEVEL still compiles to nothing either way, and
	 * one above it still pays exactly one formatter call, structured or not.
	 * Every field is escaped, including the message, so a payload containing
	 * a quote or a newline cannot break the line it appears on into two.
	 */
	bool structured = false;

	/**
	 * @brief Ring buffer of recent messages held back for @c dump_backtrace.
	 *        0 disables it.
	 *
	 * With this on, messages @em below the active level are still formatted and
	 * stored rather than discarded, so a later @c dump_backtrace can print the
	 * run-up to a failure at a detail the log itself never carried. That is the
	 * feature; it is also the cost, and the cost is not small:
	 *
	 * - A sub-level call stops being nearly free. Instead of an early level
	 *   check and return, the message is formatted into a buffer and pushed
	 * into the ring under the tracer's mutex. Enabling a backtrace makes every
	 *   @c spdlog::debug in the process do real work at level=info.
	 * - It cannot see anything the compiler removed. @c spdlog::trace below
	 *   @c SPDLOG_ACTIVE_LEVEL does not exist in the binary, so outside a Debug
	 *   build the ring captures none of the hot-path tracing core/logging.hpp
	 *   tells you to write with that macro. Capturing it requires a Debug
	 * build, or a lower floor and the cost above on every such path. @c
	 * spdlog::debug is unaffected - the floor leaves it in every config.
	 *
	 * So: useful on a cold path where the last N steps before an error matter,
	 * misleading if you expect it to have watched a hot loop it could not see.
	 */
	std::size_t backtrace = 0;

	/**
	 * @brief Hand each line to a background thread instead of formatting the
	 *        pattern and writing the sinks on the thread that logged it.
	 *
	 * On by default, and the reason is where the log calls actually come from.
	 * @c session::gate_logger and @c session::engine_logger are template
	 * parameters of @c live_session, so they run on the **producer thread** -
	 * the one that decodes a frame, runs the quoter and submits the order.
	 * Synchronously, that thread pays the pattern render, a mutex on each sink,
	 * and a @c write. Asynchronously it pays a formatted-buffer move and one
	 * enqueue.
	 *
	 * @note What this does @em not move is the message payload. spdlog formats
	 *       the caller's arguments into a buffer *before* the queue - it has
	 * to, since the arguments are references that would dangle - so
	 *       @c "{}" over a @c trade still runs on the producer thread. What
	 *       moves is the pattern (or the JSON envelope) and the sink write,
	 *       which is the larger half and the half that takes locks. Keeping the
	 *       payload cheap is still the caller's job, which is why the hot taps
	 *       check the level before they format at all.
	 *
	 * @warning A crash loses whatever is still queued. @c shutdown drains it,
	 * so an orderly exit loses nothing, but a log line is no longer evidence
	 * that the write reached the disk before the next instruction ran. Set this
	 * false to get the synchronous behaviour back while debugging something
	 * that kills the process.
	 */
	bool async = true;

	/**
	 * @brief Lines the async queue holds before the overflow policy applies.
	 *
	 * @par What happens when it fills, and why it is not "block"
	 * The oldest queued line is dropped. spdlog's other choice is to block the
	 * producer until the backend drains, and on this path blocking would put a
	 * sink write in the order-submission path - which is the exact cost the
	 * async logger exists to remove, reintroduced at the worst possible moment,
	 * because the queue fills when the process is busiest. A trading thread
	 * must not wait on a diagnostic. So the failure mode is chosen
	 * deliberately: lose the oldest diagnostics, never stall the trade.
	 *
	 * Sized against a burst rather than a rate: 8192 lines is more than a
	 * 100 ms feed cadence can produce between two drains of a backend thread
	 * that does nothing else, so a full queue means something is genuinely
	 * wrong - trace logging left on in production, or a stalled sink - and
	 * dropping is the right answer in both.
	 */
	std::size_t async_queue = 8192;
};

} // namespace exchange::core::logging
