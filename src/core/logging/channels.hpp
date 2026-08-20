#pragma once
// Named loggers, so a line says which part of the system it came from.
//
// One default logger means every line looks alike, and in a process where the
// feed, the matching engine, the risk gate and the strategy are all talking at
// once, "which of these four is this" is the first question a reader has. A
// channel answers it in the pattern rather than in the message text.
//
// --- what a channel is not -------------------------------------------------
//
// It is *not* a licence for the module it names to log. Nothing under
// `trading-engine/`, `risk_management/` or `strategy/` links spdlog, and that is
// deliberate rather than an omission: the matching thread owns books other
// people's orders are waiting on, and spdlog allocates, formats and takes a sink
// lock. A `channel::matching` line is emitted by whoever is *allowed* to log
// about the engine - the composition root, off the hot path, from the events the
// engine published - and the name says what the line is about, not which
// translation unit wrote it.
//
// That distinction is the whole reason this file is here instead of a
// `spdlog::logger` member on each module: attribution is a property of the
// message, and it needs no dependency to be true.
//
// --- how they relate to the default logger ---------------------------------
//
// Each is a clone of it: same sinks, same level, same pattern, same backtrace
// depth, taken at `init` time. So `--log-level=trace` reaches every channel,
// `--log-file` collects every channel, and a deployment configures one thing.
// A channel asked for before `init` has run clones whatever spdlog's own
// default logger is, which keeps a test that never initialises logging working.

#include "core/logging/settings.hpp"
#include "core_export.hpp" // CORE_EXPORT (generated)

#include <spdlog/logger.h>

#include <cstdint>

namespace exchange::core::logging {

/**
 * @brief What a line is about.
 *
 * Named for the subject rather than the module path, because that is what a
 * reader is scanning for: @c matching covers the engine, the books and the
 * events they publish, wherever the line was actually emitted from.
 */
enum class channel : std::uint8_t {
	matching, ///< the engine, the books, and the trades and outcomes they emit
	risk,     ///< the gate's refusals, the breaker's trips
	trading,  ///< what a strategy decided to send
	feed,     ///< market data: frames, snapshots, sequence gaps
};

/**
 * @brief The logger for @p which.
 *
 * @return A reference that stays valid for the process's life. Never null: if
 *         no channel has been installed it clones the current default logger on
 *         first use.
 *
 * @note Cheap enough for a per-event call *at a level that is off* - the
 *       reference is a pointer load and spdlog's own level check is an atomic
 *       load - but it is still worth hoisting out of a loop, because that is one
 *       load rather than one per event. Nothing here is safe to call
 *       concurrently with @c init, which must run before the threads that log
 *       are started. @see lifecycle.hpp
 */
[[nodiscard]] CORE_EXPORT spdlog::logger &logger_for(channel which) noexcept;

/**
 * @brief Install a clone of the default logger for every channel.
 *
 * Called by @c init, so a process that configures logging normally never needs
 * this. Exposed because a test that installs its own default logger wants the
 * channels to follow it rather than to keep the sinks of whatever came before.
 */
CORE_EXPORT void install_channels();

} // namespace exchange::core::logging
