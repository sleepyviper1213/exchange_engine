#pragma once
// What a strategy is, expressed so the compiler can check it and so the host can
// ask what each one subscribes to.
//
// These are the whole compile-time story. A strategy opts into a stream by
// defining the hook for it; the host tests the concept with `if constexpr` and
// only generates the fan-out loops somebody actually subscribed to. A host whose
// strategies all ignore trades has no trade loop in the object code at all —
// not a loop that iterates zero times, no loop.

#include "command_writer.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::strategy {

/**
 * @brief Reacts to executions on the tape.
 *
 * @c trade names the two orders, the price and the size — not the listing, so a
 * trade observer reads prices in the context its host was given. @see
 * command_writer
 */
template <class S>
concept trade_observer =
	requires(S &s, const engine::trade &t, command_writer &out) { s.on_trade(t, out); };

/**
 * @brief Reacts to what became of an order — acks, fills, cancels, rejects.
 *
 * This is the feedback channel. @c engine_partition::submit returns before any
 * book has seen the command, so an outcome is the only way a strategy learns
 * that the slice it showed has been taken.
 */
template <class S>
concept outcome_observer = requires(S &s, const engine::order_outcome &o,
									command_writer &out) {
	s.on_outcome(o, out);
};

/**
 * @brief Driven by the passage of time rather than by an event.
 *
 * The hook a schedule-following strategy needs — TWAP releasing a slice per
 * interval, a quoter ageing out a stale quote. Nothing in this tree implements
 * it yet, and it is declared anyway: a capability with no implementor is what
 * makes the elision testable, because a host of trade- and outcome-driven
 * strategies must report @c OBSERVES_CLOCK as false and compile @c on_clock to
 * a return.
 */
template <class S>
concept clocked = requires(S &s, std::uint64_t now_ns, command_writer &out) {
	s.on_clock(now_ns, out);
};

/**
 * @brief Declares, at compile time, how many commands one event can make it
 *        emit.
 *
 * The number the whole framework's allocation story rests on. The host sums it
 * across its strategies, sizes its buffer from the sum, and refuses to dispatch
 * an event unless that many slots are free — so a write can never need to grow
 * anything, and @c command_writer::write is a store and an increment with an
 * assert rather than a capacity branch with a fallback.
 *
 * Count the worst case, not the common one. A strategy keyed by order id sees at
 * most one of its own orders per outcome and bounds at 1; one that scans every
 * armed slot on a print can trigger all of them and bounds at its slot count.
 */
template <class S>
concept bounded_emitter = requires {
	{ S::MAX_COMMANDS_PER_EVENT } -> std::convertible_to<std::size_t>;
} && (S::MAX_COMMANDS_PER_EVENT > 0);

/// @brief A strategy: bounded, and subscribed to at least one stream. One that
///        subscribes to nothing is a configuration mistake, not a null object —
///        it would be fed forever and could never act.
template <class S>
concept runnable_strategy =
	bounded_emitter<S> &&
	(trade_observer<S> || outcome_observer<S> || clocked<S>);

/**
 * @brief Somewhere to put a finished batch — @c execution::engine_partition, or
 *        a test double.
 *
 * All-or-nothing by contract: @c false must leave the sink exactly as it was, so
 * the host can keep the batch and retry rather than having to work out which
 * prefix got through. @c spsc_queue::try_emplace_range already guarantees this,
 * which is why the signature is shaped around it.
 */
template <class T>
concept command_sink =
	requires(T &sink, std::span<const engine::event::command> batch) {
		{ sink.submit_range(batch) } -> std::same_as<bool>;
	};

} // namespace exchange::strategy
