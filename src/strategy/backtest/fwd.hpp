#pragma once
// Declarations for the offline backtest harness.
//
// A submodule of strategy/ because it exists to answer a question about a
// strategy - "what would this have done" - and because it is the only consumer
// of the strategy hooks that is allowed to be slow. Nothing on a live path
// includes it.

#include "trading-engine/orders/types.hpp"

#include <cstddef>
#include <cstdint>

namespace exchange::strategy::backtest {

class feed_clock;
class clock_view;

struct fill_model_options;
template <class Sink>
class crossing_fill_model;

struct quoter_options;
template <class Sink>
class spread_quoter;

struct report;
struct report_summary;

struct session_options;
class session;

} // namespace exchange::strategy::backtest
