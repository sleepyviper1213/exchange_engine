#pragma once
// Declarations for the offline backtest harness.
//
// A submodule of strategy/ because it exists to answer a question about a
// strategy - "what would this have done" - and because it is the only consumer
// of the strategy hooks that is allowed to be slow. Nothing on a live path
// includes it, which is why the reference trader is no longer declared here:
// `serve` drives the same quoter a backtest does, so it belongs to strategy/
// proper. @see strategy/quoter.hpp

namespace exchange::strategy::backtest {

class feed_clock;
class clock_view;

template <class T>
class delay_queue;

struct latency_model;
// `wire` is deliberately absent, for the reason strategy/fwd.hpp gives about
// `strategy_engine`: its clock parameter is constrained, and a declaration that
// drops the constraint declares a different template rather than this one.
// Naming it means including wire.hpp anyway.

class queue_position_book;

struct fill_model_options;
template <class Sink>
class crossing_fill_model;

struct report;
struct report_summary;

struct session_options;
class session;

} // namespace exchange::strategy::backtest
