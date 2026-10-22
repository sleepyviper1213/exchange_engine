#pragma once
// The offline backtest harness: a recorded venue against this process's own
// matching engine. See docs/backtesting.md for what it models and what it does
// not.
//
// Kept out of strategy.hpp deliberately. A backtest pulls in market_data, the
// execution partition and the risk gate; a translation unit that merely defines
// a strategy should pay for none of that. Ask for this header by name.
//
// format.hpp is absent for the same reason one level down - printing a report
// costs <fmt/format.h>, so it is the caller's to include.

// IWYU pragma: begin_exports
#include "backtest/fwd.hpp"
#include "backtest/markout_recorder.hpp"
#include "backtest/markout_report.hpp"
#include "backtest/modelled_fill.hpp"
#include "backtest/null_trader.hpp"
#include "backtest/report.hpp"
#include "backtest/session.hpp"
#include "backtest/tape_audit.hpp"
#include "backtest/tape_audit_report.hpp"
#include "simulation.hpp"

// IWYU pragma: end_exports
