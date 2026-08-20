#pragma once
// The push side of a gate: who gets told when a rule refuses a command, when
// the breaker trips, and when the sink pushes back.
//
// Everything a gate knows is already readable - `refused()`, `breaches(rule)`,
// `rejections()`, `stalls()`. Those are counters and a buffer, which is the
// right shape for an operator's console polling once a second and the wrong one
// for anything that has to act on the *event*: a log line naming the order, a
// metric incremented per rule, a pager when the breaker cuts the line. Polling
// cannot see a refusal that was overwritten before the next read, and
// `rejections()` deliberately holds only the last batch.
//
// So the gate takes an observer, in the shape `strategy/concepts.hpp` already
// established: a hook is a member function, opting in is defining it, and the
// gate tests each with `if constexpr`. An observer that defines none of them is
// a configuration mistake rather than a null object - `no_observer` is how you
// spell "nothing", and it is the default.
//
// --- what is deliberately not here ----------------------------------------
//
// There is no on_pass and no on_deliver. The accepted path is the one under a
// nanosecond budget, and its whole design is that ten rules cost ten compares
// and one branch; hanging a call off it would be paid on every order to report
// the case nobody investigates. Every hook below fires from a path that had
// already left the arithmetic - a refusal that is building an outcome record, a
// trip, a rollback - so an attached observer cannot slow the fast path down. It
// is also why `passed()` has no hook and never will: count it, do not narrate
// it.
//
// --- why the hooks are noexcept -------------------------------------------
//
// Required, not requested: `roll_back` is noexcept, and an exception escaping a
// hook there would unwind through a gate whose ledger is half retired. The
// project's rule is that exceptions belong to startup, configuration and I/O,
// and this is none of those - an observer that logs is an observer that
// catches. The concepts spell the requirement so a throwing hook fails at the
// gate's instantiation instead of at the worst possible moment.

#include "fwd.hpp"
#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/system/trading_state.hpp" // IWYU pragma: export
#include "trading-engine/event/command.hpp"

#include <concepts>
#include <cstddef>

namespace exchange::risk::hooks {

/**
 * @brief Told which rules refused a command, as the refusal happens.
 *
 * @par What the mask carries that an outcome does not
 * A refused command produces at most one @c order_outcome, carrying the *one*
 * @c reject_reason a client is told - the lowest set bit. @see RISK_BREACH_LIST
 * The set handed here is every rule the command broke, which is what a human
 * diagnosing a strategy wants and what @c first_reason throws away.
 *
 * @par Fired once per refusal, and never for one that did not happen
 * The hook runs from the commit half of @c submit_range, after the sink has
 * accepted delivery. A batch the sink refused is rolled back and re-screened,
 * so its refusals are reported when the retry lands rather than once per
 * attempt - the hook's call count therefore matches @c refused() exactly.
 *
 * @note Anonymous commands reach the hook too. An ADD or a REDUCE names no
 *       order, so @c rejections() cannot describe it and does not try; this is
 *       the only way a refused seed is ever visible.
 */
template <class O>
concept breach_observer =
	requires(O &o, const engine::event::command &cmd, breach_set reasons) {
		{ o.on_breach(cmd, reasons) } noexcept;
	};

/**
 * @brief Told when the gate itself stops the line.
 *
 * Only the gate's *own* trips - the breach-rate cut-out and the loss floor.
 * An operator throwing the switch by hand does not come through here: the
 * breaker is shared and this gate may not even be running when that happens, so
 * a hook that claimed to see every trip would be lying. @c circuit_breaker is
 * the thing to ask about state; this is the thing that says "and it was us".
 *
 * @par Why both arguments
 * The state says trading stopped and the cause says what to do about it, and
 * they are genuinely different questions. @see trip_cause
 */
template <class O>
concept halt_observer =
	requires(O &o, system::trading_state to, system::trip_cause why) {
		{ o.on_halt(to, why) } noexcept;
	};

/**
 * @brief Told when the sink refused delivery and the batch was rolled back.
 *
 * Saturation, not error - the host still holds the batch and will hand it back.
 * Worth a hook because a gate that stalls is a gate whose partition is behind,
 * and the number of commands held back is the depth of that hole.
 *
 * @note Fires per rollback, not per command, and before the retry. A hook that
 *       logs unconditionally will log once per attempt for as long as the sink
 *       stays full, which is the honest reading of a stall and also a reason to
 *       rate-limit whatever it writes to.
 */
template <class O>
concept stall_observer = requires(O &o, std::size_t retained) {
	{ o.on_stall(retained) } noexcept;
};

/**
 * @brief An observer that opts into nothing, and the gate's default.
 *
 * Empty, so a gate holding one is not one byte larger than a gate without -
 * @c risk_gate stores it under @c [[no_unique_address]] the way it already
 * stores a stateless clock - and every notification compiles to nothing at all,
 * not to a call that returns.
 */
struct no_observer {};

/**
 * @brief What takes an @c Observer parameter accepts: at least one hook, or
 *        @c no_observer.
 *
 * Two things do - @c risk_gate and @c system::heartbeat_monitor - and they
 * subscribe to different subsets, which is the reason the individual concepts
 * exist rather than one interface: the monitor can only ever halt, so it tests
 * @c halt_observer and ignores the rest.
 *
 * Same reasoning as @c strategy::runnable_strategy, and it earns its keep for
 * the same reason: hooks are detected with @c if constexpr, so a misspelled
 * @c on_breech would compile, never fire, and look exactly like a gate that
 * refuses nothing. Requiring at least one hook turns that typo into an error at
 * the point the gate is declared. Deliberate silence spells itself.
 */
template <class O>
concept risk_observer = std::same_as<O, no_observer> || breach_observer<O> ||
						halt_observer<O> || stall_observer<O>;

} // namespace exchange::risk::hooks
