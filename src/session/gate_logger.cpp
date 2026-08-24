#include "gate_logger.hpp"

#include "core/logging/channels.hpp"
#include "event/format.hpp" // IWYU pragma: keep - fmt::formatter<command>
#include "risk_management/format.hpp" // IWYU pragma: keep - fmt::formatter<breach_set>

#include <fmt/base.h>

namespace exchange::session {

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

gate_logger::gate_logger() noexcept
	: log_(&core::logging::logger_for(core::logging::channel::risk)) {}

gate_logger::gate_logger(spdlog::logger &to) noexcept : log_(&to) {}

void gate_logger::on_breach(const engine::event::command &cmd,
							risk::hooks::breach_set reasons) const noexcept {
	if (log_->should_log(spdlog::level::debug)) emit_breach(cmd, reasons);
}

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

} // namespace exchange::session
