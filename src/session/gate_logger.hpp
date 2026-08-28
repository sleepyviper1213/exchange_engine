#pragma once

#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/system/fwd.hpp"
#include "session_export.hpp"
#include "event/command.hpp"

#include <spdlog/logger.h>

namespace exchange::session {

/**
 * @brief Logs what the risk gate refused, and why it stopped.
 *
 * @par Why an observer rather than reading the counters
 * Because a counter cannot name the order. @c risk_gate::breaches tells an
 * operator that the price band fired forty times; only the hook says *which*
 * command, carrying every rule it broke rather than the one a client is told.
 * That set is what a human diagnosing a strategy wants, and @c first_reason
 * throws the rest away.
 *
 * @note Every hook is @c noexcept, which the concepts require rather than
 *       request: @c roll_back is @c noexcept, and an exception escaping here
 *       would unwind a gate whose ledger is half retired. An observer that logs
 *       is an observer that catches, and the .cpp is where that happens -
 *       spdlog routes *sink* failures through its own error handler, but
 *       formatting still allocates, so the promise has to be kept rather than
 *       assumed.
 */
class gate_logger {
public:
	/// @brief Log to the risk channel. @see core::logging::channel
	SESSION_EXPORT
	gate_logger() noexcept;

	/// @brief Log somewhere else - a test double, or a deployment that wants
	///        refusals on a sink of their own.
	/// @param to Must outlive every gate this is copied into.
	SESSION_EXPORT
	explicit gate_logger(spdlog::logger &to) noexcept;

	/**
	 * @brief One refused command, with every rule it broke.
	 *
	 * The level check is inline and the formatting is not, which is the one
	 * place in this file that distinction earns anything: @c risk_gate calls
	 * this from @c commit's loop, once per refused command, inside the same
	 * @c submit_range a strategy calls. Refusals are not rare in every
	 * configuration - a fat-finger band rejects the deep end of a venue's
	 * published depth on every frame - so at @c info this has to cost an atomic
	 * load and a not-taken branch rather than a call into another translation
	 * unit that then decides not to log.
	 *
	 * @note Every other gate in the tree pays nothing at all for this, and not
	 *       because of the guard: @c risk_gate wraps the call in
	 *       @c if constexpr (breach_observer<Observer>), so a gate with
	 *       @c no_observer has no call site to guard.
	 */
	SESSION_EXPORT
	void on_breach(const engine::event::command &cmd,
				   risk::hooks::breach_set reasons) const noexcept;

	/// @brief The breaker changed what it is letting through.
	///
	/// Unguarded, and out of line: a trip happens once per episode and a human
	/// then decides whether to re-arm, so there is nothing here worth inlining
	/// a check for.
	SESSION_EXPORT
	void on_halt(risk::hooks::system::trading_state to,
				 risk::hooks::system::trip_cause why) const noexcept;

	/// @brief The sink pushed back and the batch was rolled back for a retry.
	///        Saturation, not error - which is why it is debug and not warn.
	SESSION_EXPORT
	void on_stall(std::size_t retained) const noexcept;

private:
	/// @brief The formatting half of @c on_breach. Out of line so the header
	///        needs no formatter, and reached only once the level says the line
	///        will actually be written.
	void emit_breach(const engine::event::command &cmd,
					 risk::hooks::breach_set reasons) const noexcept;

	spdlog::logger *log_;
};

} // namespace exchange::session
