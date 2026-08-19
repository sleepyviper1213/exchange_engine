#pragma once
// `exchange_tool live` - keep a local L2 replica of a venue's book in step with
// its live feed, and report on how well that went.
//
// The online counterpart to `replay`: same reconstruction, same gap detection,
// same book, but the events arrive from a socket and the snapshots that repair
// them have to be fetched while the events keep coming. @see app/live_feed.hpp
// for the pipeline that overlaps the two.

#include <string>
#include <string_view>

namespace exchange::app {

/**
 * @brief Track @p symbol's published depth live for @p seconds.
 *
 * @param symbol Binance symbol to subscribe to.
 * @param seconds How long to run; 0 runs until the stream ends.
 * @param speed Venue update cadence - @c "100ms" or @c "1000ms".
 * @param limit Levels per side to request in each REST snapshot.
 * @param price_decimals Tick precision for the symbol.
 * @param qty_decimals Step precision for the symbol.
 * @param depth Ladder rows per side to print at the end; 0 prints all of them.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_live(const std::string &symbol, int seconds, std::string_view speed,
			 int limit, int price_decimals, int qty_decimals, int depth);

} // namespace exchange::app
