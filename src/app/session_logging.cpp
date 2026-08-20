#include "session_logging.hpp"

#include "risk_management/format.hpp" // IWYU pragma: keep - fmt::formatter<breach_set>
#include "trading-engine/format.hpp" // IWYU pragma: keep - fmt::formatter<command>, <trade>, <order_outcome>

#include <utility>

namespace exchange::app {
namespace {

/**
 * @brief Log one line through @p to, and swallow anything the attempt throws.
 *
 * One place rather than a @c try in each hook, and it is not defensive padding:
 * @c spdlog::logger::log is not @c noexcept - formatting a message allocates -
 * and every hook below promises to be. The concepts demand that promise because
 * @c risk_gate::roll_back is @c noexcept, and an exception escaping a hook
 * would unwind a gate whose ledger is half retired.
 */
template <class... Args>
void say(spdlog::logger &to, spdlog::level::level_enum at,
		 fmt::format_string<Args...> pattern, Args &&...args) noexcept {
	// Swallowed because there is nothing to report a failed log line *with*:
	// the reporting channel is what just failed. Losing a diagnostic beats
	// unwinding a caller that is mid-rollback.
	try {
		to.log(at, pattern, std::forward<Args>(args)...);
	} catch (...) {} // NOLINT(bugprone-empty-catch)
}

} // namespace

// --- gate_logger ----------------------------------------------------------

gate_logger::gate_logger() noexcept
	: log_(&core::logging::logger_for(core::logging::channel::risk)) {}

gate_logger::gate_logger(spdlog::logger &to) noexcept : log_(&to) {}

void gate_logger::emit_breach(const engine::event::command &cmd,
							  risk::hooks::breach_set reasons) const noexcept {
	// The level has already been checked by the inline half, so this formats
	// unconditionally - `say` is here for the promise about throwing, not for a
	// second look at the level.
	say(*log_, spdlog::level::debug, "refused {} - {}", cmd, reasons);
}

void gate_logger::on_halt(risk::hooks::system::trading_state to,
						  risk::hooks::system::trip_cause why) const noexcept {
	say(*log_, spdlog::level::warn, "breaker -> {} ({})", to, why);
}

void gate_logger::on_stall(std::size_t retained) const noexcept {
	say(*log_,
		spdlog::level::debug,
		"sink full, {} commands held for retry",
		retained);
}

// --- engine_logger --------------------------------------------------------

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

} // namespace exchange::app
