#pragma once
// `exchange_tool replay` — rebuild the venue's published depth from a capture.
//
// market-data only: the target is `l2_book` throughout and the matching engine
// is not involved, because none of this is our order flow. `backtest` is the
// command that runs the same file through the engine.

#include <string>

namespace exchange::app {

/**
 * @brief Reconstruct published depth from a JSONL diff capture.
 *
 * @param file The capture to replay.
 * @param snapshot_file Non-empty to seed the book from a saved REST snapshot.
 * @param price_decimals Scale the venue quotes prices on.
 * @param qty_decimals Scale the venue quotes sizes on.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_replay(const std::string &file, const std::string &snapshot_file,
			   int price_decimals, int qty_decimals);

} // namespace exchange::app
