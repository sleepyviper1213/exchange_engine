#pragma once

#include "core/logging/settings.hpp"
#include "core_export.hpp" // CORE_EXPORT (generated)

// Installing, draining, and tearing down the process-wide logger.
//
// No <spdlog/spdlog.h> here either: none of these declarations names a spdlog
// type, so a translation unit that only starts logging up need not compile the
// library that does the logging. Include core/logging.hpp to emit messages.
namespace exchange::core::logging {

/**
 * @brief Install the process-wide default logger.
 *
 * Call once, early, before anything else logs - messages emitted beforehand go
 * to spdlog's own default logger and miss the file sink. Calling it again
 * replaces the default logger, which is what a test that wants a quiet run
 * needs.
 *
 * The console sink is @b stderr, not stdout: a log carries diagnostics, while
 * stdout carries whatever the program is actually for. Keeping them apart is
 * what lets a caller redirect one without the other.
 *
 * @param config Level, file, logger name, and backtrace depth.
 *
 * @warning Once this has been called, nothing may log from the destructor of an
 *          object with static storage duration. Those run after @c main, by
 *          which point the installed logger and its sinks have themselves been
 *          destroyed, and the call is a use-after-free - measured as a
 * segfault, not a silently dropped message. It is specifically @c init that
 *          introduces this: a process that never initialises logging survives
 *          the same destructor, because spdlog's own built-in default logger is
 *          still standing. Nothing in this codebase logs from a static
 *          destructor today; keep it that way.
 *
 * @warning Not thread-safe against concurrent logging. Installing the default
 *          logger races with any other thread reading it, so this must be
 * called once, before the threads that will log are started.
 */
CORE_EXPORT void init(const settings &config);

/**
 * @brief Emit the held-back backtrace ring, then clear it. No-op when
 *        @c logging_settings::backtrace was 0.
 *
 * Call it where a failure is being reported and the steps leading to it are
 * worth having: the ring is written out at its own level, so messages the log
 * would otherwise never have shown appear together, in order, at the moment
 * they became interesting.
 *
 * @note Safe to call from any thread. spdlog's tracer holds its own mutex, and
 *       the ring's contents are moved out under it - concurrent producers block
 *       briefly rather than race. That mutex is also why this belongs on an
 *       error path and not in a loop.
 */
CORE_EXPORT void dump_backtrace() noexcept;

/**
 * @brief Push every buffered message to its sinks, keeping the logger usable.
 *
 * spdlog only flushes automatically at warn and above, so info and below can
 * sit in a buffer for an unbounded time. Call this at any point the log needs
 * to be up to date on disk - before a long blocking operation, or before a
 * crash is expected.
 *
 * @note Under @c settings::async - the default - this @em enqueues the flush
 *       rather than performing it, so the sinks are up to date shortly after
 *       the call rather than on return. @c shutdown is the one that drains
 *       synchronously, by joining the thread pool. Anything that must observe
 *       the log file complete at a particular instant - a test reading it
 *       back - needs that or a synchronous logger, not this.
 */
CORE_EXPORT void flush() noexcept;

/**
 * @brief Flush every sink and drop the default logger.
 *
 * @warning After this, nothing may log again. @c spdlog::default_logger()
 *          returns null and the next @c spdlog::info dereferences it, which is
 * a segfault, not a no-op. That makes this the wrong thing to call at the end
 * of @c main in any process holding a static object whose destructor logs -
 * those run afterwards.
 *
 * @par Under settings::async this must be reached before main returns
 * An async logger owns a worker thread, held by spdlog's registry, which is a
 * static. Leave it alive and the join happens during *static* destruction,
 * which on Windows runs under the loader lock - while the worker needs that
 * same lock to finish exiting. Neither proceeds, and the process hangs after it
 * has done its work and returned. Measured on MinGW, with the main thread
 * parked in @c thread_pool::~thread_pool. Called from ordinary running code, as
 * here, the same join completes, because no loader lock is held.
 *
 * @c guard does this at the end of its scope, so declaring one in @c main is
 * the whole answer for a normal program. Call it directly only where there is
 * no guard to do it - a test binary whose @c main belongs to gtest, or code
 * reconfiguring sinks wholesale.
 */
CORE_EXPORT void shutdown() noexcept;

} // namespace exchange::core::logging
