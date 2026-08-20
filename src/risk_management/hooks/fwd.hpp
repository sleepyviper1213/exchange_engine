#pragma once

// The interception points, one file per rule, and the map from the industry's
// vocabulary to this tree.
//
// A "risk hook" in an HFT stack is a place in the tick-to-trade path where an
// order can be stopped: order size, position, rate, price collar, duplicate
// state on the way in, the ones that watch what came back - fills, messages per
// fill, acknowledgements that stopped arriving - and the out-of-band ones -
// kill switch, drawdown breaker, feed liveness - that stop trading rather than
// stopping one order. This submodule is that list, made explicit, because the
// list is a compliance artefact as much as a design: an operator asks "which
// checks run before an order leaves, and what watches it afterwards" and the
// answer should be a directory listing, not a scroll through a 700-line class.
//
// ```text
// hooks/
// ├── breach.hpp                what a hook found wrong, as bits
// ├── observer.hpp              who is told when a hook fires
// ├── feedback.hpp              the return path that keeps the state honest
// ├── detail/screening.hpp      what a batch carries into the rules
// ├── detail/fixed_window.hpp   the counting window the rate-shaped rules share
// ├── pre_trade/                per command, on the hot path
// │   ├── order_size_check.hpp  quantity, and price x quantity
// │   ├── price_collar.hpp      the fat-finger band, and the band itself
// │   ├── position_limit.hpp    net position and gross exposure, projected
// │   ├── position.{hpp,cpp}      the book that projection reads
// │   ├── rate_limiter.{hpp,cpp}  this window's allowance, and the rule
// │   ├── duplicate.{hpp,cpp}   an id already working, and the ledger's ceiling
// │   └── working_ledger.{hpp,cpp} the ledger it reserves in
// ├── post_trade/               per published event, off the hot path
// │   ├── limits.hpp               the thresholds, kept out of risk_limits
// │   ├── order_trade_ratio.{hpp,cpp} messages per execution
// │   ├── fill_burst.{hpp,cpp}     executions per window, and a price run
// │   ├── outcome_silence.{hpp,cpp} exposure the return path stopped mentioning
// │   └── monitor.{hpp,cpp}        the one owner per listing
// └── system/                   out-of-band, and they stop everything
//     ├── global_kill_switch.hpp   what a tripped breaker refuses
//     ├── circuit_breaker.{hpp,cpp} the switch all three of these trip
//     ├── trading_state.hpp        what it says, and why it stopped
//     ├── pnl_drawdown_breaker.{hpp,cpp} the loss floor
//     └── heartbeat.{hpp,cpp}      silence from the venue
// ```
//
// --- what these are, and what they are not --------------------------------
//
// Each rule is a *function of its inputs* returning the bits of the rules it
// found broken - not an object with a virtual `check`, and not something that
// owns state. That is the one design decision this layout has to
// get right, because the obvious reading of "a chain of hooks" is a vector of
// polymorphic checks called in a loop, and that would cost an indirect call and
// an unpredictable branch per rule on the path budgeted in nanoseconds.
//
// Instead `risk_gate` calls all of them, unconditionally, and ORs their results
// into one mask: ten rules cost ten compares and one branch, exactly as before
// the split. The rules were already written that way inside the gate; this
// gives each one a name, a header, a docstring and a test of its own without
// changing a single instruction. @see risk_gate::place_limits for the
// composition.
//
// The state each hook measures against lives *with* it - the ledger the
// duplicate rule reserves in, the window the rate rule reads, the position book
// the exposure projection is measured against, the breaker all three system
// hooks trip. So a hook never reaches back up the tree for the thing it is
// about, and the module root is left holding only what every hook shares: the
// policy, the clock, the breach vocabulary and the gate that composes them.
//
// And the filing is spelled at the call site rather than hidden behind an
// alias: it is `hooks::pre_trade::position_book` and
// `hooks::system::circuit_breaker`, or just `pre_trade::` and `system::` from
// inside this namespace. A flat `risk::circuit_breaker` reads as module
// vocabulary and hides which lane owns the type, which is the one thing a
// reader tracing dependencies actually needs. @see risk_management/fwd.hpp
//
// --- the three lanes, and why the directories differ ----------------------
//
// `pre_trade/` runs per command inside `submit_range` and answers "may this
// order go". `post_trade/` runs per published event on the dispatcher thread
// and answers "was what just happened the right shape" - a question no single
// command can be asked, because its subject is a sequence. `system/` runs on
// its own schedule - a print, a poll, an operator - and answers "may anything
// go".
//
// The last two both end the same way, and that is the load-bearing part: they
// trip the `circuit_breaker` that every pre-trade check already reads as one
// byte in `screen_state`. So neither lane costs the submit path anything, and
// neither needs the submit path to know it exists. That is why the drawdown
// breaker is not a pre-trade rule and why an order-to-trade ratio is not
// either: a losing position and a thousand-to-one quote ratio are both facts
// about history, not about the order in front of you, so refusing that order
// while accepting the next identical one would be incoherent.
//
// The three lanes are also three cadences and three budgets. `pre_trade/` is
// nanoseconds and branchless arithmetic; `post_trade/` may loop and keep
// history because nothing waits on it; `system/` is whatever its driver's
// schedule is. A uniform "hook engine" interface across the three would have
// made the first pay for the generality of the other two. @see
// post_trade/fwd.hpp
//
// --- deliberately absent -------------------------------------------------
//
// *Self-trade prevention.* It wants an account id on `order`, which does not
// exist yet. @see TODO.md #10
//
// *The kill switch's mass-cancel.* Tripping stops new orders; it does not pull
// the ones already resting, and pulling them means iterating `working_ledger`,
// which has no iteration. @see hooks/system/global_kill_switch.hpp
//
// *A firm-wide rate limit and a portfolio-wide position limit.* Both are
// aggregations across gates and belong above one. @see TODO.md #11
//
// *Reconciliation against the venue's own view of what is working.* The
// post-trade lane measures the return path going quiet, which is as close as a
// process with no drop-copy feed can get. @see post_trade/outcome_silence.hpp

namespace exchange::risk::hooks {

// The observer policy's null case. Its concepts cannot be declared here -
// a concept has no forward declaration - and neither can `feedback_router` or
// `heartbeat_monitor`, whose parameters are constrained: repeating them without
// the constraint declares a different template rather than referring to these.
// Same reason `risk_gate` is absent from the module's own fwd.hpp.
struct no_observer;

} // namespace exchange::risk::hooks
