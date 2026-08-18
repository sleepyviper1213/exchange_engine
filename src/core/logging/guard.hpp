#pragma once

#include "core_export.hpp" // CORE_EXPORT (generated)

#include "core/logging/settings.hpp"

// The scoped form of init/flush.
namespace exchange::core::logging {

/**
 * @brief Scoped logging: @c init on construction, @c flush on destruction.
 *
 * Two honest limits on what it buys, both measured rather than assumed:
 *
 * - It does @b not rescue messages that would otherwise be lost. A normal exit
 *   already flushes the sinks through the C runtime, with or without this; a
 *   hard exit (@c _Exit, @c abort) runs no destructors, so it skips this too.
 *   What the guard adds is a @em deterministic flush point - the end of its
 *   scope - instead of whenever the runtime gets round to it.
 * - It deliberately does @b not call @c shutdown, which would leave the default
 *   logger null and turn any later log call into a null dereference. Flushing
 *   reaches the same sinks without that edge.
 *
 * @code
 * int main(int argc, char **argv) {
 *     const core::logging::guard log{{.level       = "info",
 *                                     .log_file    = "exchange_tool.log",
 *                                     .logger_name = "exchange_tool"}};
 *     ...              // flushed at the end of main, on every return path
 * }
 * @endcode
 *
 * @warning One per process, declared before anything logs. Constructing a second
 *          one replaces the process-wide logger the first installed, which is
 *          rarely what a nested scope intends.
 */
class guard {
public:
	CORE_EXPORT explicit guard(const settings &config);
	CORE_EXPORT ~guard();

	// Owns process-wide state, so neither copyable nor movable: two objects
	// believing they own it is the nested-scope bug above.
	guard(const guard &)            = delete;
	guard &operator=(const guard &) = delete;
	guard(guard &&)                 = delete;
	guard &operator=(guard &&)      = delete;
};

} // namespace exchange::core::logging
