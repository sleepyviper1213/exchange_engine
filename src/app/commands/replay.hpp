#pragma once
// `exchange_tool replay` - rebuild the venue's published depth from a capture.
//
// market_data only: the target is `l2_book` throughout and the matching engine
// is not involved, because none of this is our order flow. `backtest` is the
// command that runs the same file through the engine.

#include <string>

namespace exchange::app {

/**
 * @brief Reconstruct published depth from a JSONL diff capture.
 *
 * Runs the managed-local-order-book procedure, not a bare apply loop: events go
 * through @c depth_reconstructor, so ones the snapshot already covers are
 * discarded rather than written over it, and a missing frame is reported as a
 * gap instead of silently corrupting the replica.
 *
 * @param file The capture to replay.
 * @param snapshot_file Non-empty to seed the book from a saved REST snapshot.
 *        Empty is legal and now means the book stays *empty* rather than
 *        partial: with nothing to sequence against, every event buffers.
 * @param price_decimals Scale the venue quotes prices on.
 * @param qty_decimals Scale the venue quotes sizes on.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged. A gap in
 *         the recording is reported loudly but is not a failure of this
 *         command; failing to reach the end of the file is.
 */
int cmd_replay(const std::string &file, const std::string &snapshot_file,
			   int price_decimals, int qty_decimals);

} // namespace exchange::app
