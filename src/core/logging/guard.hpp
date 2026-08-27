#pragma once

#include "core/logging/settings.hpp"
#include "core_export.hpp" // CORE_EXPORT (generated)

// The scoped form of init/flush.
namespace exchange::core::logging {

/**
 * @brief Scoped logging: @c init on construction, @c shutdown on destruction.
 *
 * @par Why the scope must end the logger rather than only flush it
 * Because @c settings::async gives the logger a worker thread, and that thread
 * has to be joined somewhere. Left to the process, the join happens during
 * static destruction - which on Windows runs under the loader lock, while the
 * worker needs that same lock to finish exiting. The two wait on each other and
 * the process hangs after @c main has returned. Ending the logger with the
 * scope is what keeps the join on an ordinary running thread, where it
 * completes. @see logging::shutdown
 *
 * @warning The default logger is null once this destructs, so nothing may log
 *          afterwards - a later @c spdlog::info is a null dereference, not a
 *          no-op. In practice that means declaring the guard in @c main and
 *          keeping log calls inside its scope, which is the shape below.
 *          Nothing in this codebase logs from a static destructor; keep it that
 *          way. @see logging::init's warning on the same hazard.
 *
 * @note It does @b not rescue messages that would otherwise be lost. A normal
 *       exit already flushes the sinks through the C runtime, with or without
 *       this; a hard exit (@c _Exit, @c abort) runs no destructors, so it skips
 *       this too. What the guard adds is a deterministic end point - the end of
 *       its scope - instead of whenever the runtime gets round to it.
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
 * @warning One per process, declared before anything logs. Constructing a
 * second one replaces the process-wide logger the first installed, which is
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
