#pragma once
// `exchange_tool snapshot` — fetch or load a venue depth snapshot and print the
// top of book.

#include <string>

namespace exchange::app {

/**
 * @brief Fetch (or load) a Binance depth snapshot and print top of book.
 *
 * @param symbol Binance symbol to fetch, ignored when @p file is given.
 * @param file A saved depth JSON to load instead of going to the network.
 * @param limit REST depth limit.
 * @param price_decimals Scale the venue quotes prices on.
 * @param qty_decimals Scale the venue quotes sizes on.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_snapshot(const std::string &symbol, const std::string &file, int limit,
				 int price_decimals, int qty_decimals);

} // namespace exchange::app
