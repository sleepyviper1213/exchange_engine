#pragma once
// `exchange_tool backtest` — the same capture as `replay`, but through the
// whole engine.
//
// `replay` above reconstructs the venue's published depth and stops there —
// market-data only, no matching, no orders. This runs the *rest* of the system
// over the same file: the depth becomes resting liquidity in a real order_book,
// a trader quotes into a real risk gate, the matching engine executes what
// crosses, and a fill model supplies the one thing the recording cannot (see
// strategy/backtest/fill_model.hpp). The output is a report, not a book.

#include <cstdint>
#include <string>

namespace exchange::app {

/// @brief Everything `backtest` was asked for, gathered so the driver's
///        signature stays readable. @see add_backtest
struct backtest_settings {
	std::string file;                  ///< JSONL capture of depthUpdate frames
	std::string snapshot;              ///< REST depth JSON to seed the replica
	std::string symbol   = "BACKTEST"; ///< display name for the listing
	std::string tick     = "0.01";     ///< price increment, as decimal text
	std::string lot      = "0.01";     ///< size increment, as decimal text
	int price_decimals   = 2;
	int qty_decimals     = 2;
	std::uint64_t events = 0; ///< stop after this many frames; 0 is all of them
	std::int64_t max_position = 0; ///< lots; 0 leaves the position limit open
	int improve_ticks         = 1; ///< how far inside the touch to quote
	int lots                  = 1; ///< quote size, in lots
	int requote_ms            = 0; ///< market time a quote is left standing
	bool fill_on_lock         = false; ///< fill on a locked market, not only a
									   ///< trade-through
	bool quote = true;                 ///< run the reference quoter at all
};

/**
 * @brief Run @p settings' capture through the engine and print a report.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_backtest(const backtest_settings &settings);

} // namespace exchange::app
