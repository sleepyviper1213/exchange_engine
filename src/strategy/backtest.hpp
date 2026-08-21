#pragma once
// The offline backtest harness: a recorded venue against this process's own
// matching engine. See docs/backtesting.md for what it models and what it does
// not.
//
// Kept out of strategy.hpp deliberately. A backtest pulls in market-data, the
// execution partition and the risk gate; a translation unit that merely defines
// a strategy should pay for none of that. Ask for this header by name.
//
// format.hpp is absent for the same reason one level down - printing a report
// costs <fmt/format.h>, so it is the caller's to include.

// IWYU pragma: begin_exports
#include "backtest/clock.hpp"
#include "backtest/fill_model.hpp"
#include "backtest/fwd.hpp"
#include "backtest/queue_position.hpp"
#include "backtest/report.hpp"
#include "backtest/scheduler.hpp"
#include "backtest/session.hpp"
#include "backtest/wire.hpp"
// IWYU pragma: end_exports
