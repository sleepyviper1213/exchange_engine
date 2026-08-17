#pragma once
// `exchange_tool capture` — record the venue's published diff-depth feed to a
// file, for `replay` and `backtest` to read back.

#include <string>
#include <string_view>

namespace exchange::app {

/**
 * @brief Record the `<symbol>@depth` stream to @p outfile for @p seconds.
 *
 * @param symbol Binance symbol to subscribe to.
 * @param outfile JSONL file the frames are appended to.
 * @param seconds How long to record. Must be positive.
 * @param speed Venue update cadence — @c "100ms" or @c "1000ms".
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_capture(const std::string &symbol, const std::string &outfile,
				int seconds, std::string_view speed);

} // namespace exchange::app
