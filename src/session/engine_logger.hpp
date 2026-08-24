#pragma once
// What a live session says while it is running, and where each line comes from.
// `engine_logger` fills the fan-out's `Watcher` slot, so it sees the trades
// and outcomes the partition published, on the thread that routes them
// rather than the one that produced them. @see app/feedback_fanout.hpp
//
// Neither of them takes a record apart. Every type below prints itself - the
// module that owns a value owns how it reads, and `{}` on a `trade`, an
// `order_outcome`, a `command` or a `breach_set` reaches the formatter in that
// module's own `format.hpp` sidecar. A log line that spelled out somebody
// else's fields would go stale the moment they changed, and would say something
// different from every other line about the same type.
#include "session_export.hpp"
#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"

#include <spdlog/logger.h>

#include <span>

namespace exchange::session {

/**
 * @brief Logs the trades and outcomes the engine published.
 *
 * @par Where this runs, which is the only interesting thing about it
 * On the thread that *routes* the events, not the one that matched them. The
 * matching thread put them in an @c event_channel and went back to matching; by
 * the time a line is written the batch is a copy on the far side of a ring.
 * That is what makes it safe to format a string per trade at all, and it is why
 * there is no version of this that lives in @c matching_engine.
 *
 * @note Trace rather than debug, and per event rather than per batch. A busy
 *       listing publishes thousands of these a second, so this is a level you
 *       turn on to answer a question and turn off again - the per-batch summary
 *       an operator watches continuously is the periodic progress line in
 *       @c cmd_serve.
 */
class engine_logger {
public:
	/// @brief Log to the matching channel. @see core::logging::channel
	SESSION_EXPORT engine_logger() noexcept;

	/// @param to Must outlive every fan-out this is copied into.
	SESSION_EXPORT explicit engine_logger(spdlog::logger &to) noexcept;

	SESSION_EXPORT
	void on_trades(std::span<const engine::trade> executions) const noexcept;

	SESSION_EXPORT void
	on_outcomes(std::span<const engine::order_outcome> records) const noexcept;

private:
	spdlog::logger *log_;
};

} // namespace exchange::session
