#pragma once
// `exchange_tool serve` - the live path: a venue's feed, this process's engine,
// and a run that stays up.
//
// The three commands above it each hold one part of this and stop there. `live`
// reconstructs a venue's depth and prints a ladder - market_data only, no
// matching. `demo` runs the full producer/consumer engine loop but against
// synthetic flow, so nothing it matches came from a market. `backtest` runs the
// whole chain including the risk gate, but over a recording and in one thread,
// with a fill model supplying the one thing a recording cannot.
//
// This is the join: a live feed driving the real depth bridge, a strategy
// through the real risk gate, a matching engine on its own thread, and the
// published events routed back to the gate, the strategy and the post-trade
// monitor.
//
// By default it is still not an order gateway: the venue supplies prices and
// depth, and the matching happens in this process against liquidity seeded from
// what the venue publishes, which is what `depth_feed_bridge` exists for.
// `--send-orders` is what changes that, and it changes it in both directions -
// the quoter's orders go out through `session::venue_gateway`, and the venue's
// executions come back through its account stream and are booked by the same
// risk gate that screened them. Nothing is sent without that flag, and the flag
// is refused unless an environment was named.
//
// The wiring is `app/live_session.hpp`, where a test can drive it without a
// socket. This file is the shim that gives it an io_context, a signal handler,
// a consumer thread and a metrics timer.

#include "core/metrics/settings.hpp"
#include "venue/credentials.hpp"
#include "venue/environment.hpp"

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
	std::string speed = "100ms"; ///< diff-stream cadence

	/**
	 * @brief Accept the feed's TLS certificate without checking it.
	 *
	 * Off, so the depth stream is verified. The feed carries no credential, and
	 * that is the reason this reader went unverified for as long as it did -
	 * but it is the wrong reading. An intercepted feed does not leak anything;
	 * it *dictates* the book this process believes in, and every quote and
	 * every order that follows comes out of that book. So it is verified like
	 * the credentialed path, and this exists for the one case verification
	 * cannot serve: a host with no CA bundle installed, where the handshake
	 * fails outright. Asked for by name, never inferred from a failure.
	 */
	bool insecure_tls = false;

	/**
	 * @brief Read the tick and step from the venue rather than from the flags
	 *        below.
	 *
	 * On by default, because the flags' defaults are wrong for almost every
	 * listing and wrong *silently*: surplus decimals are truncated on the way
	 * in, so a step configured coarser than the venue's rounds every level
	 * below it to zero and still reports a clean parse. SOLUSDT's step is
	 * 0.001, ETHUSDT's 0.0001, BTCUSDT's 0.00001 - against a default of 0.01.
	 *
	 * Clearing it uses @c tick, @c lot and the two decimal counts as given,
	 * which is what a run against a recording or a venue that cannot be reached
	 * needs. A failed fetch falls back to the flags with a warning rather than
	 * refusing to start. @see fetch_venue_grid
	 */
	bool venue_grid = true;

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

	// --- simulated passive execution ---------------------------------------
	/**
	 * @brief Infer the fills a *resting* order would have taken.
	 *
	 * The one switch here that makes a run something other than the production
	 * chain, which is why it is off by default and why the report says so. It
	 * is what makes `--quote` measurable: without it a passive strategy fills
	 * against nothing, because seeded depth is rested without matching.
	 * @see live_session_options::simulate_fills
	 */
	bool simulate_fills = false;

	/// @brief Fill on a locked market, not only on a trade-through. Strictly
	///        more optimistic. Read only with @c simulate_fills.
	bool fill_on_lock = false;

	/// @brief Treat every order as first in line at its price. Read only with
	///        @c simulate_fills.
	bool front_of_queue = false;

	// --- the modelled network ----------------------------------------------
	/**
	 * @brief Wall-clock nanoseconds a command spends in flight before the
	 *        engine has it. Zero puts no wire in the chain at all.
	 *
	 * The same flag `backtest` has, and it is the same flag deliberately: a
	 * live number and a replayed one are only comparable if the order path cost
	 * the same in both. @see live_session_options::latency
	 *
	 * @warning Sub-millisecond values are not faithfully reproducible here, and
	 *          the reason is the platform rather than the model. Delivery is
	 *          driven by a steady timer whose resolution is a millisecond or so
	 *          on Windows, so a modelled 50 us arrives late and jittered. A
	 *          backtest has no such floor - it moves market time itself - which
	 *          is why the two agree on the arithmetic and not on the precision.
	 */
	std::uint64_t latency_ns = 0;

	/// @brief Uniform extra flight time, drawn once per message. Same caveat
	///        about resolution as @c latency_ns.
	std::uint64_t jitter_ns = 0;

	/// @brief Seed for the jitter draw; zero keeps the model's own.
	std::uint64_t seed = 0;

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
	/// @brief market_data silence that trips the breaker. Zero disables. Sized
	///        against the diff cadence: two missed frames is a diagnosis.
	std::uint64_t feed_timeout_ms = 0;

	// --- venue credential --------------------------------------------------

	/**
	 * @brief API key and secret, read from the environment and nowhere else.
	 *
	 * Empty unless @c BINANCE_API_KEY and @c BINANCE_API_SECRET are set, which
	 * is the ordinary case: `serve` runs the engine against a live feed and
	 * matches internally, so it needs no credential to do its job. What the
	 * credential decides is whether this run *could* place an order, which is
	 * worth reporting at startup either way. @see app/credentials_option.hpp
	 */
	venue::credentials credential{};

	// --- order entry -------------------------------------------------------

	/**
	 * @brief Which deployment of the venue this run talks to.
	 *
	 * Drives the depth stream, the snapshot fetch, the reference-data read
	 * *and* the order path, from one value, because they have to agree:
	 * testnet keeps its own book, so a run reading production depth while
	 * placing orders there is a strategy reacting to a market it is not
	 * trading in. @see venue::environment
	 *
	 * Production by default, and that is about the feed rather than about
	 * orders: reading the real book is what `serve` has always done and is the
	 * only setting in which its numbers mean anything. It is safe as a default
	 * only because @c send_orders is not - nothing is sent unless it is set,
	 * and setting it without naming an environment is refused.
	 */
	venue::environment env = venue::environment::production;

	/**
	 * @brief Whether an environment was actually named on the command line.
	 *
	 * Separate from @c env because the default above is indistinguishable from
	 * @c --live once it has been resolved, and those two must not be the same
	 * thing to @c --send-orders. A flag rather than making @c env optional: the
	 * feed needs an answer either way, and only order entry needs to know
	 * whether anybody chose it.
	 */
	bool env_chosen = false;

	/**
	 * @brief Send this session's orders to the venue rather than only matching
	 *        them internally.
	 *
	 * Off, and this is the flag that decides whether `serve` is a measurement
	 * or a participant. With it clear the engine matches against liquidity
	 * seeded from published depth and nothing leaves the process - which is
	 * what every run of this command did before there was a gateway at all.
	 *
	 * @note Refused without an explicit @c --testnet, @c --demo or @c --live,
	 *       so the production default above can never quietly become production
	 *       *order entry*. @see cmd_serve
	 */
	bool send_orders = false;

	/// @brief Orders this run may place in total; zero is unlimited. The blunt
	///        bound on a strategy that quotes in a loop. @see gateway_limits
	std::uint32_t max_orders = 0;

	/**
	 * @brief Rate-limit weight to leave unspent for market data.
	 *
	 * Order entry and depth snapshots compete for one per-IP allowance, and a
	 * quoter that spends it all is a quoter whose next resync cannot be
	 * fetched - so the feed dies to keep the orders flowing, which is exactly
	 * backwards. Sized well above what a run of snapshots costs.
	 * @see venue::BINANCE_SPOT_WEIGHT_PER_MINUTE
	 */
	int weight_reserve = 600;
};

/**
 * @brief Run the live path until it is stopped, and print what it did.
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged.
 */
int cmd_serve(const serve_settings &settings,
			  const core::metrics::settings &metrics_settings);

} // namespace exchange::app
