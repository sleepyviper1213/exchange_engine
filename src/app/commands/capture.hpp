#pragma once
// `exchange_tool capture` - record one of the venue's published market-data
// streams to a file, for `replay`, `backtest` and `trades` to read back.

#include <string>
#include <string_view>

namespace exchange::app {

/**
 * @brief Record a Binance market-data stream to @p outfile for @p seconds.
 *
 * @param symbol Binance symbol to subscribe to.
 * @param outfile JSONL file the frames are appended to.
 * @param seconds How long to record. Must be positive.
 * @param speed Venue update cadence - @c "100ms" or @c "1000ms". Depth only;
 *        the tape has no cadence to choose, because the venue pushes a message
 *        per fill rather than on a timer.
 * @param stream Which stream to record - @c "depth" or @c "trade".
 * @param insecure_tls Skip verification of the venue's TLS certificate. For a
 *        host with no CA bundle; a recording taken over an unverified stream is
 *        what every later replay and backtest is measured against, so this is
 *        asked for by name rather than defaulted to.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 *
 * @note The two streams are separate subscriptions and therefore separate
 *       recordings. Capturing both for one window means running this twice,
 *       concurrently - and it is worth knowing that nothing then relates the
 *       two files but their timestamps, since a print carries no update id.
 */
int cmd_capture(const std::string &symbol, const std::string &outfile,
				int seconds, std::string_view speed, std::string_view stream,
				bool insecure_tls = false);

} // namespace exchange::app
