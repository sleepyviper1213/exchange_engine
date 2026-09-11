#pragma once
// Loading the REST depth payload that seeds a replay or a backtest.
//
// Three commands take a `--snapshot` and all three used to spell the same
// slurp-then-parse, so all three reported the same thing when it went wrong:
// `bids: missing or mistyped field`. That is a true statement about the JSON
// and a useless one about the mistake, which is almost always the same mistake
// - passing the diff capture where the snapshot was wanted. The two are both
// JSON files full of price levels sitting next to each other in a directory,
// and nothing about their names says which is which.
//
// So the diagnosis lives here, once, and it is made by *decoding* rather than
// by sniffing for substrings: if the file's first line parses as a depthUpdate
// frame, it is a capture, and there is no heuristic left to be wrong about.

#include "market_data/binance/fwd.hpp"

#include <optional>
#include <string>

namespace exchange::app {

/**
 * @brief Read @p path as a Binance REST depth payload.
 *
 * @param path File holding the `/api/v3/depth` response as the venue served it.
 * @param price_decimals Tick precision for the symbol.
 * @param qty_decimals Step precision for the symbol.
 * @return The parsed snapshot, or @c std::nullopt.
 *
 * @note Logs the reason itself, at error level, including a diagnosis when the
 *       file turns out to be a diff capture. Callers return @c EXIT_FAILURE and
 *       add nothing - the point of the helper is that the message is written
 *       once, where the evidence for it is.
 */
[[nodiscard]] std::optional<market_data::binance::depth_snapshot>
load_seed_snapshot(const std::string &path, int price_decimals,
				   int qty_decimals);

} // namespace exchange::app
