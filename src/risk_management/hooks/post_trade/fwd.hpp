#pragma once

// The third lane: what watches the orders that already left.
//
// `pre_trade/` answers "may this order go" and runs inside `submit_range`, per
// command, on the tick-to-trade path. `system/` answers "may anything go" and
// runs on somebody else's schedule. Neither can see the failures that are
// properties of a *sequence* rather than of a command or of a clock: a strategy
// quoting a thousand times per fill, a resting side being filled far faster
// than it was meant to be, a working set the venue has stopped talking about.
// Every order involved in those passed every pre-trade rule, one at a time,
// correctly. The pattern is only visible after the fact.
//
// ```text
// post_trade/
// ├── limits.hpp                  thresholds, kept out of risk_limits
// ├── order_trade_ratio.{hpp,cpp} messages per execution, and what counts
// ├── fill_burst.{hpp,cpp}        executions per window, and a price run
// ├── outcome_silence.{hpp,cpp}   exposure nothing is coming back about
// └── monitor.{hpp,cpp}           the one owner per listing, and its feed
// ```
//
// --- why this lane is an object and the other two are not ------------------
//
// `pre_trade/` has a composer already: `risk_gate` calls every rule
// unconditionally and ORs the bits, which is why ten rules cost one branch.
// `system/` has no composer at all and wants none - its three hooks are driven
// by three unrelated signals and compose through the state they all write,
// which is `circuit_breaker`. This lane is the one that genuinely has a single
// owner, a single input and a loop: it consumes the whole published event
// stream for a listing, in order, on one thread. That is what
// `post_trade_monitor` is, and `hooks::feedback_router` is what drives it. @see
// monitor.hpp
//
// --- the budget is different, and that is the point -----------------------
//
// Nothing here runs on the submit path. It runs on the dispatcher thread, after
// the fact, against events that have already been published - so it may keep
// history, it may loop, and a microsecond spent here costs nothing that a
// strategy measures. What it must not do is *become* a pre-trade rule: the
// moment one of these is consulted per command, the per-command budget owns it.
// So each of them terminates in a `circuit_breaker` trip, which the pre-trade
// lane already reads for free as one byte in `screen_state`.
//
// --- what these rules can and cannot see ----------------------------------
//
// The lane's whole input is what a partition publishes: `engine::trade` and
// `engine::order_outcome`. That is less than it sounds, and the limits are
// load-bearing rather than incidental.
//
// A `trade` carries two order ids, a price and a volume - and *no side*. Which
// side of a print was ours is a question for the gate's `working_ledger`, and
// reaching for it from here would either duplicate the ledger or point an edge
// from this lane at the gate that owns it. So a rule here that wants to say
// "the market ran through our bids" says "the tape ran one way through prints
// we were part of" instead, and `fill_burst` documents the gap rather than
// papering over it.
//
// An `order_outcome` is the closest thing to "a message the venue processed",
// but it is not one-for-one: a FILL is not a message, and a CANCELLED carrying
// `TIME_IN_FORCE` is the book withdrawing an IOC remainder rather than anyone
// having sent a cancel. `order_trade_ratio` owns that counting policy, because
// it is the only rule that depends on it.
//
// --- deliberately absent --------------------------------------------------
//
// *Per-order ageing.* "This specific order has rested for a minute with no
// outcome" needs a walk over `working_ledger`, which has no iteration - it is
// an open-addressed probe table built for one lookup per command. So the
// staleness rule measures the aggregate instead: exposure outstanding and
// nothing coming back at all. That catches the failure that matters, which is
// the return path dying, and misses the one order out of a thousand that got
// lost on its own. @see outcome_silence.hpp
//
// *Reconciliation against a drop copy.* The honest version of "the gate's
// ledger and the venue disagree" compares against the venue's own view, and
// this process has no drop-copy feed to compare with. @see TODO.md #11
//
// *Wash trades and self-crossing.* A surveillance pattern that needs an account
// id on `order`, which does not exist yet - the same blocker as self-trade
// prevention on the way in. @see TODO.md #10
//
// *Per-account attribution.* Same blocker, and the same shape: this lane is one
// monitor per listing because that is the unit `position_book` and
// `risk_limits` are already denominated in.

#include <cstdint>

namespace exchange::risk::hooks::post_trade {

struct post_trade_limits;

class order_trade_ratio;
class fill_burst;
class outcome_silence;
class post_trade_monitor;

// Defined with its X-macro list in fill_burst.hpp, the way trading_state is in
// its own header: the list that generates an enum's names is part of the enum
// and travels with it. @see hooks/system/fwd.hpp
enum class tape_direction : std::uint8_t;

} // namespace exchange::risk::hooks::post_trade
