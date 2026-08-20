#pragma once
// What a live session says while it is running, and where each line comes from.
//
// Nothing under `trading-engine/`, `risk_management/` or `strategy/` logs - the
// matching thread cannot afford spdlog, and the risk gate's whole design is
// that a hook nobody defined costs nothing. So every line about them is emitted
// from here, through the extension points those modules already offer:
//
//   * `gate_logger` fills the gate's `Observer` slot. `risk_gate` calls it from
//     the *commit* half of `submit_range`, after the sink has accepted delivery
//     and while it is already building an outcome record - never from the
//     branchless arithmetic. @see risk_management/hooks/observer.hpp
//   * `engine_logger` fills the fan-out's `Watcher` slot, so it sees the trades
//     and outcomes the partition published, on the thread that routes them
//     rather than the one that produced them. @see app/feedback_fanout.hpp
//
// --- why these hold a logger rather than looking one up -------------------
//
// Because the alternative is a member function that never touches `this`, which
// clang-tidy is right to complain about
// (`readability-convert-member-functions-to-static`). Making the hooks `static`
// trades that warning for a worse one: both slots are filled by *value* and
// called through the instance, so `risk_gate::notify_breach` and
// `feedback_fanout::on_trades` would then be reaching a static member through
// an object (`readability-static-accessed-through-instance`) - and requiring
// static hooks would narrow both concepts to observers that can never hold
// state.
//
// Holding the `spdlog::logger *` makes both checks correct instead of trading
// one for the other, and it is the shape `observer.hpp` already asks for: "one
// that has to outlive the call carries a handle to whatever it reports to
// rather than the state itself". It is also less work per batch - the channel
// is resolved once at construction rather than on every line.
//
// Neither of them takes a record apart. Every type below prints itself - the
// module that owns a value owns how it reads, and `{}` on a `trade`, an
// `order_outcome`, a `command` or a `breach_set` reaches the formatter in that
// module's own `format.hpp` sidecar. A log line that spelled out somebody
// else's fields would go stale the moment they changed, and would say something
// different from every other line about the same type.
//
// Both are level-gated by spdlog, so a run at `info` pays an atomic load per
// batch and nothing else. Neither is on the matching thread and neither can
// block it.
//
// The bodies are in the .cpp, and this header names only `spdlog::logger` -
// through `<spdlog/logger.h>`, the light half, which is what `channels.hpp`
// includes for the same reason. The formatters each line needs are heavier than
// the declarations that use them, so they go with the code that formats.

#include "core/logging/channels.hpp"
#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/system/trading_state.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"

#include <spdlog/logger.h>

#include <cstddef>
#include <span>

namespace exchange::app {

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
	gate_logger() noexcept;

	/// @brief Log somewhere else - a test double, or a deployment that wants
	///        refusals on a sink of their own.
	/// @param to Must outlive every gate this is copied into.
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
	void on_breach(const engine::event::command &cmd,
				   risk::hooks::breach_set reasons) const noexcept {
		if (log_->should_log(spdlog::level::debug)) emit_breach(cmd, reasons);
	}

	/// @brief The breaker changed what it is letting through.
	///
	/// Unguarded, and out of line: a trip happens once per episode and a human
	/// then decides whether to re-arm, so there is nothing here worth inlining
	/// a check for.
	void on_halt(risk::hooks::system::trading_state to,
				 risk::hooks::system::trip_cause why) const noexcept;

	/// @brief The sink pushed back and the batch was rolled back for a retry.
	///        Saturation, not error - which is why it is debug and not warn.
	void on_stall(std::size_t retained) const noexcept;

private:
	/// @brief The formatting half of @c on_breach. Out of line so the header
	///        needs no formatter, and reached only once the level says the line
	///        will actually be written.
	void emit_breach(const engine::event::command &cmd,
					 risk::hooks::breach_set reasons) const noexcept;

	spdlog::logger *log_;
};

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
	engine_logger() noexcept;

	/// @param to Must outlive every fan-out this is copied into.
	explicit engine_logger(spdlog::logger &to) noexcept;

	void on_trades(std::span<const engine::trade> executions) const noexcept;

	void
	on_outcomes(std::span<const engine::order_outcome> records) const noexcept;

private:
	spdlog::logger *log_;
};

} // namespace exchange::app
