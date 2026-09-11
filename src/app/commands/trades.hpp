#pragma once
// `exchange_tool trades` - read a JSONL trade capture back as a tape.
//
// The counterpart of `replay`, and deliberately not a book: a print says what
// traded, not what is resting, so there is nothing here to reconstruct. What it
// reports instead is the shape of the arrival process - rate, burstiness, and
// how hard prints cluster onto a few prices - because those are the properties
// a recording exists to establish and the ones a synthetic feed gets wrong.

#include <string>

namespace exchange::app {

/**
 * @brief Replay a JSONL capture of @c trade frames and report the tape.
 *
 * @param file The capture to read.
 * @param price_decimals Scale the venue quotes prices on.
 * @param qty_decimals Scale the venue quotes sizes on.
 * @param bucket_ms Width of the buckets the rate distribution is measured over.
 *        Must be positive.
 * @param max_trades Stop after this many prints, or 0 for the whole file.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_trades(const std::string &file, int price_decimals, int qty_decimals,
			   int bucket_ms, unsigned long long max_trades);

} // namespace exchange::app
