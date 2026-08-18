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
	 *   tells you to write with that macro. Capturing it requires a Debug build,
	 *   or a lower floor and the cost above on every such path. @c spdlog::debug
	 *   is unaffected - the floor leaves it in every config.
	 *
	 * So: useful on a cold path where the last N steps before an error matter,
	 * misleading if you expect it to have watched a hot loop it could not see.
	 */
	std::size_t backtrace = 0;
};

} // namespace exchange::core::logging
