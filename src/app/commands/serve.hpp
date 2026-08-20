#pragma once
// `exchange_tool serve` - the live path: a venue's feed, this process's engine,
// and a run that stays up.
//
// The three commands above it each hold one part of this and stop there. `live`
// reconstructs a venue's depth and prints a ladder - market-data only, no
// matching. `demo` runs the full producer/consumer engine loop but against
// synthetic flow, so nothing it matches came from a market. `backtest` runs the
// whole chain including the risk gate, but over a recording and in one thread,
// with a fill model supplying the one thing a recording cannot.
//
// This is the join: a live feed driving the real depth bridge, a strategy
// through the real risk gate, a matching engine on its own thread, and the
// published events routed back to the gate, the strategy and the post-trade
// monitor. What it is *not* is an order gateway - nothing here sends an order
// to Binance. The venue supplies prices and depth; the matching happens in this
// process, against liquidity seeded from what the venue publishes, which is
// what `depth_feed_bridge` exists for.
//
// The wiring is `app/live_session.hpp`, where a test can drive it without a
// socket. This file is the shim that gives it an io_context, a signal handler,
// a consumer thread and a metrics timer.

#include "core/metrics/settings.hpp"

#include <cstdint>
#include <string>

namespace exchange::app {

/// @brief Everything `serve` was asked for, gathered so the driver's signature
///        stays readable. @see add_serve
struct serve_settings {
	/// @brief Levels a REST snapshot asks for. Under what the replica retains
	///        (128 a side), so nothing is fetched only to be dropped.
	static constexpr int DEFAULT_SNAPSHOT_LIMIT = 100;

	/// @brief Pause before rebuilding a dropped stream. Not zero: a venue that
	///        just closed on us is not helped by an immediate retry, and a
	///        tight loop against a rate-limited endpoint gets an address banned
	///        rather than connected.
	static constexpr std::uint64_t DEFAULT_RECONNECT_MS = 500;

	// --- the listing and the feed -----------------------------------------
	std::string symbol = "SOLUSDT"; ///< trading pair on the venue
	std::string tick   = "0.01";    ///< price increment, as decimal text
	std::string lot    = "0.01";    ///< size increment, as decimal text
	/**
	 * @brief Session anchor for the listing's price collar, as decimal text.
	 *
	 * Read only when a collar is configured, and `serve` configures none - so
	 * the default is the smallest on-grid value rather than a guess about where
	 * the instrument trades. The fat-finger band that *is* live here is
	 * `--price-band`, which the risk gate measures around the last print rather
	 * than around a session anchor. @see risk_limits::price_band_bps
	 */
	std::string reference;
	std::string speed          = "100ms"; ///< diff-stream cadence
	int price_decimals         = 2;
	int qty_decimals           = 2;
	int limit                  = DEFAULT_SNAPSHOT_LIMIT;
	int seconds                = 0; ///< run for this long; 0 is until stopped
	std::uint64_t reconnect_ms = DEFAULT_RECONNECT_MS;
	std::size_t max_reconnects = 0; ///< attempts before giving up; 0 is keep

	// --- the strategy ------------------------------------------------------
	int improve_ticks = 1;    ///< ticks inside the touch to quote
	int lots          = 1;    ///< order size, in lots
	int requote_ms    = 0;    ///< market time an order is left standing
	bool take         = true; ///< cross the touch instead of resting inside it

	// --- pre-trade risk ----------------------------------------------------
	std::int64_t max_position      = 0; ///< lots; 0 leaves the limit open
	std::int64_t max_order_qty     = 0; ///< lots; 0 leaves the limit open
	std::int64_t price_band_bps    = 0; ///< fat-finger band; 0 disables
	std::int64_t max_loss          = 0; ///< tick-lots; 0 disables the breaker
	std::uint32_t breaches_to_trip = 0; ///< refusals per window; 0 is manual

	// --- post-trade surveillance -------------------------------------------
	std::uint32_t max_messages_per_execution = 0; ///< OTR cap; 0 disables
	std::uint32_t max_executions_per_window  = 0; ///< burst cap; 0 disables
	std::uint32_t max_adverse_run            = 0; ///< tape run cap; 0 disables
	std::uint64_t outcome_timeout_ms         = 0; ///< return-path silence

	/**
	 * @brief Width of the burst window, in milliseconds. Zero keeps the
	 *        library default of about 1 ms.
	 *
	 * Exposed because without it @c max_executions_per_window is unreachable at
	 * a diff feed's cadence: frames arrive about ten times a second, each take
	 * fills once, so a 1 ms window never holds more than one execution and no
	 * cap above zero can ever be crossed. A cap wants a window it can fill.
	 *
	 * @note Rounded *up* to a power of two - @c fixed_window indexes by a
	 * shift, which is the whole reason none of these rules divides. Up rather
	 *       than down so a window is never quietly narrower than the one asked
	 *       for; the effective width is logged.
	 */
	std::uint64_t burst_window_ms = 0;

	/// @brief Width of the order-to-trade window, in milliseconds. Zero keeps
	///        the default of about 1.07 s. Same rounding, and the same reason
	///        for existing: an OTR measured over a second is measured over
	///        eight or so messages, which is not a statement about anything.
	std::uint64_t otr_window_ms = 0;

	/// @brief Messages a window must hold before the ratio is judged. Zero
	///        keeps the default of 100. Below it the rule declines to answer,
	///        which is why a short window plus the default floor is a rule that
	///        can never fire. @see post_trade_limits::min_messages_to_judge
	std::uint32_t min_otr_messages = 0;

	// --- the system lane ---------------------------------------------------
	/// @brief Market-data silence that trips the breaker. Zero disables. Sized
	///        against the diff cadence: two missed frames is a diagnosis.
	std::uint64_t feed_timeout_ms = 0;
};

/**
 * @brief Run the live path until it is stopped, and print what it did.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_serve(const serve_settings &settings,
			  const core::metrics::settings &metrics_settings);

} // namespace exchange::app
