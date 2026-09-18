#pragma once
// Global Kill Switch: what a tripped breaker refuses, and what it does not.
//
// The switch itself is `risk_management/circuit_breaker.hpp` - one atomic byte,
// written by an operator's thread or by the gate's own automatic trips, read by
// everyone. This file is its *interception point*: the two rules that turn a
// state into a refusal, which is where the switch stops being a variable and
// starts stopping orders.
//
// Out-of-band by construction. The state is not carried on the command path and
// nothing has to be threaded through a strategy for a trip to take effect: the
// breaker is shared, the gate reads it once per batch, and an operator flipping
// it from another thread is seen by the next batch without any handshake. That
// is the whole reason it is an atomic and not a member of the gate.

#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/detail/screening.hpp" // bit_if
#include "risk_management/hooks/fwd.hpp"
#include "risk_management/hooks/system/trading_state.hpp"

namespace exchange::risk::hooks::system {

/**
 * @brief The bit a non-@c NORMAL state sets for a command that *adds* risk - a
 *        client order, or an anonymous seed.
 *
 * @return @c HALTED, or zero.
 *
 * @note Any state but @c NORMAL refuses these, which is what makes
 *       @c CANCEL_ONLY the useful trip: it stops the strategy adding to a
 *       position without stopping it shedding one.
 */
[[nodiscard]] inline breach_bits
new_liquidity_breach(trading_state state) noexcept {
	return bit_if(state != trading_state::NORMAL, breach::HALTED);
}

/**
 * @brief The bit that stops a *risk-reducing* command - a cancel or a
 * reduction.
 *
 * @return @c HALTED when the state is @c HALTED, and zero otherwise -
 *         @c CANCEL_ONLY deliberately lets these through.
 *
 * @par Why a halt that blocks cancels is a separate, hand-selected state
 * A kill switch that blocks withdrawals freezes a malfunctioning strategy's
 * orders in the book and leaves them to be filled by whoever noticed. That is
 * the wrong emergency behaviour, so no automatic trip ever selects @c HALTED;
 * @c CANCEL_ONLY is what the drawdown breaker, the breach-rate cut-out and the
 * heartbeat monitor all choose. @c HALTED exists for the narrower case where
 * the strategy is not trusted to name the right orders - a bad deploy
 * cancelling ids it invented - and then the positions are unwound by hand from
 * the other side.
 * @see trading_state
 */
[[nodiscard]] inline breach_bits
risk_reducing_breach(trading_state state) noexcept {
	return bit_if(state == trading_state::HALTED, breach::HALTED);
}

// --- the other half, and where it lives ------------------------------------
//
// A real kill switch does two things: it stops new orders, and it *pulls the
// ones already resting*. The two rules above are the first. The second is a
// mass cancel - walk everything the gate believes is working and emit a CANCEL
// for each - and it is `risk_gate::mass_cancel`, because the walk needs the
// ledger, the listing and the sink, and all three are the gate's.
//
// It is not here, and it is not a rule. Everything in `hooks/` is a function of
// its inputs returning the bits it found broken; a mass cancel decides nothing
// and *emits*, which is a different kind of thing and belongs with the state it
// reads.
//
// It is also not automatic. Nothing watches the breaker and cancels on a trip -
// a composition calls it, the way one polls `heartbeat_monitor` - so a run loop
// that never calls it never mass cancels. That is worth knowing *before* the
// incident rather than during it, which is why it is said here and in
// `risk_gate::mass_cancel` rather than left to be discovered.
//
// The obstacle this used to name is gone. `working_ledger` had no iteration,
// and adding one looked like a choice between moving the slot encoding out of
// `working_ledger.cpp` - where it is deliberately sealed - and keeping a second
// ordered index beside the table. It was neither: `working_ledger::snapshot`
// copies into a caller's buffer through a non-template function, so the loop
// and the unpacking both stay sealed and nothing is indexed twice.
// @see risk_gate::mass_cancel, working_ledger::snapshot

} // namespace exchange::risk::hooks::system
