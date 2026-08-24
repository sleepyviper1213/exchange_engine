#include "engine_logger.hpp"

#include "core/logging/channels.hpp"
#include "order_book/format.hpp"

namespace exchange::session {

engine_logger::engine_logger() noexcept
	: log_(&core::logging::logger_for(core::logging::channel::matching)) {}

engine_logger::engine_logger(spdlog::logger &to) noexcept : log_(&to) {}

void engine_logger::on_trades(
	std::span<const engine::trade> executions) const noexcept {
	// Checked once for the batch rather than once per event: a span of a
	// hundred trades at `info` should cost one atomic load, not a hundred. The
	// `try` is per batch for the same reason - a throw abandons the rest of
	// this batch's tracing, which is the correct amount of damage for a
	// diagnostic.
	if (!log_->should_log(spdlog::level::trace)) return;
	try {
		for (const engine::trade &print : executions) log_->trace("{}", print);
	} catch (...) {} // NOLINT(bugprone-empty-catch) - see say
}

void engine_logger::on_outcomes(
	std::span<const engine::order_outcome> records) const noexcept {
	if (!log_->should_log(spdlog::level::trace)) return;
	try {
		for (const engine::order_outcome &record : records)
			log_->trace("{}", record);
	} catch (...) {} // NOLINT(bugprone-empty-catch) - see say
}

} // namespace exchange::session
