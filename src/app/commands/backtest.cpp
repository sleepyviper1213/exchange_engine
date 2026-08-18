#include "backtest.hpp"

#include "core/logging.hpp"
#include "core/util/slurp.hpp"
#include "market-data/format.hpp" // IWYU pragma: keep - fmt::formatter<feed_run>
#include "market_data.hpp"
#include "strategy/backtest.hpp"
#include "strategy/backtest/format.hpp" // IWYU pragma: keep - fmt::formatter<report_summary>
#include "trading-engine.hpp"

#include <fmt/std.h>
#include <spdlog/stopwatch.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace exchange::engine;
using namespace exchange::engine::orders;

namespace exchange::app {
namespace {

/// @brief Parse a tick or lot size from decimal text onto @p scale.
/// @return The scaled increment, or nothing - the reason is logged.
std::optional<std::int64_t> increment(std::string_view text, int scale,
									  std::string_view what) {
	const auto scaled = parse_exact_decimal(text, scale);
	if (!scaled) {
		spdlog::error("--{} '{}': {}", what, text, describe(scaled.error()));
		return std::nullopt;
	}
	if (*scaled <= 0) {
		spdlog::error("--{} must be positive (got '{}')", what, text);
		return std::nullopt;
	}
	return *scaled;
}

/**
 * @brief Binds a trader to a session so the pair reads as a @c feed_handler.
 *
 * @c session::on_event takes the trader as a second argument, because a trader
 * writes into the session's own gate and so cannot exist before it - see the
 * note on why the trader is a parameter of the methods rather than of the
 * class. A @c feed_handler takes only the message. This is where the trader
 * gets bound, and that is all it is: the binding, not a layer.
 *
 * Templated on the trader for the same reason @c session's own hooks are: an
 * absent @c on_market compiles away rather than becoming a branch, and the
 * quoter's hooks stay direct calls. @see backtest::session
 */
template <exchange::strategy::backtest::trader Trader>
struct session_handler {
	exchange::strategy::backtest::session *run;
	Trader *actor;
	/// @brief Whether the last snapshot left the replica live.
	bool live = false;

	void on_event(market_data::depth_event event) {
		run->on_event(std::move(event), *actor);
	}

	void on_snapshot(market_data::book_snapshot snapshot) {
		live = run->on_snapshot(std::move(snapshot), *actor);
	}
};

/**
 * @brief Drive @p run over @p feed with @p actor, then finalise the report.
 *
 * The loop itself is @c market_data::drive - the same one a live feed would be
 * driven through - so what is left here is the two things a backtest adds: the
 * trader binding above, and the tail call that runs out whatever the last event
 * left in flight.
 * @return What the feed did, and why it stopped.
 */
template <exchange::strategy::backtest::trader Trader>
market_data::feed_run
drive_backtest(exchange::strategy::backtest::session &run,
			   market_data::binance::jsonl_depth_feed &feed,
			   std::uint64_t limit, Trader &actor) {
	session_handler<Trader> handler{.run = &run, .actor = &actor};
	const market_data::feed_run replayed =
		market_data::drive(feed, handler, limit);
	if (!handler.live)
		spdlog::warn("the seed snapshot did not bring the replica live; it may "
					 "predate the capture");
	run.finish(actor);
	return replayed;
}

} // namespace

int cmd_backtest(const backtest_settings &settings) {
	namespace binance  = market_data::binance;
	namespace backtest = exchange::strategy::backtest;
	using exchange::core::util::slurp;

	// --- reference data, before anything is parsed against it ---------------
	const auto tick_scaled =
		increment(settings.tick, settings.price_decimals, "tick");
	const auto lot_scaled =
		increment(settings.lot, settings.qty_decimals, "lot");
	if (!tick_scaled || !lot_scaled) return EXIT_FAILURE;

	auto seed_json = binance::parse_binance_depth(slurp(settings.snapshot),
												  settings.price_decimals,
												  settings.qty_decimals);
	if (!seed_json) {
		spdlog::error("snapshot parse failed for {}: {}",
					  settings.snapshot,
					  seed_json.error());
		return EXIT_FAILURE;
	}
	if (seed_json->bids.empty() || seed_json->asks.empty()) {
		spdlog::error("the seed snapshot is one-sided; a backtest needs a "
					  "two-sided book to mark and to quote against");
		return EXIT_FAILURE;
	}

	// The collar is measured around this, and symbol_spec asserts it is on the
	// tick grid - reference data is a deployment fact, so a misaligned one is a
	// crash rather than a rejection. Round the snapshot's midpoint down.
	const std::int64_t mid =
		(seed_json->bids.front().price + seed_json->asks.front().price) / 2;
	const std::int64_t reference = mid - mid % *tick_scaled;
	if (reference <= 0) {
		spdlog::error("the snapshot's midpoint ({}) is below one tick", mid);
		return EXIT_FAILURE;
	}

	const symbol_spec spec{0,
						   settings.symbol,
						   settings.price_decimals,
						   settings.qty_decimals,
						   *tick_scaled,
						   *lot_scaled,
						   reference};

	// --- the capture --------------------------------------------------------
	const std::string jsonl = slurp(settings.file);
	if (jsonl.empty()) {
		spdlog::error("cannot read {} (missing or empty)", settings.file);
		return EXIT_FAILURE;
	}
	// Decoded frame by frame rather than up front. A session's capture is
	// gigabytes, none of it is wanted twice, and the seed rides the same stream
	// so the whole run is one drive loop over one venue-neutral seam. The
	// consequence is that a damaged capture is no longer diagnosed before the
	// run starts - it stops the run where the damage is, and is reported below.
	binance::jsonl_depth_feed feed(binance::normalise(std::move(*seed_json)),
								   jsonl,
								   settings.price_decimals,
								   settings.qty_decimals);
	spdlog::info("backtesting {} from {} (tick {}, lot {})",
				 settings.symbol,
				 settings.file,
				 settings.tick,
				 settings.lot);

	// --- the run ------------------------------------------------------------
	backtest::session_options options;
	options.fills.require_trade_through = !settings.fill_on_lock;
	if (settings.max_position > 0)
		options.limits.max_position_lots = settings.max_position;

	const spdlog::stopwatch watch;
	backtest::session run(spec, options);
	market_data::feed_run replayed;

	if (settings.quote) {
		backtest::spread_quoter quoter(
			run.sink(),
			spec,
			backtest::quoter_options{
				.improve_ticks = static_cast<price_t>(settings.improve_ticks),
				.lots          = static_cast<quantity_t>(settings.lots),
				.requote_interval_ns =
					static_cast<std::uint64_t>(settings.requote_ms) *
					1'000'000U,
			});
		replayed = drive_backtest(run, feed, settings.events, quoter);
		spdlog::info("quoter placed {} quotes, {} commands accepted, {} stalls",
					 quoter.quotes(),
					 quoter.submitted(),
					 quoter.stalls());
		// A run that quoted nothing is the one outcome the report above cannot
		// explain - every counter in it is zero either way. These are the only
		// two reasons the quoter has for standing aside, so they are worth a
		// line rather than a shrug.
		if (quoter.quotes() == 0)
			spdlog::warn(
				"the quoter never quoted: {} events had no room inside the "
				"spread for --improve {}, and {} had a touch off the tick grid",
				quoter.no_room(),
				settings.improve_ticks,
				quoter.off_grid());
	} else {
		// No order flow at all: this replays the capture through the bridge,
		// the partition and the matching engine and checks the harness rather
		// than a strategy. Every fill counter must come back zero.
		backtest::null_trader idle;
		replayed = drive_backtest(run, feed, settings.events, idle);
	}

	spdlog::info("replayed {} frames in {:.3f}s of wall clock: {}",
				 feed.frames(),
				 watch.elapsed().count(),
				 replayed);

	// The result, on stdout and unadorned, because something downstream may be
	// diffing two of these against each other. @see the note at the top of this
	// file on results versus commentary.
	fmt::println("{}", backtest::report_summary{&run.result(), &spec});

	const backtest::report &result = run.result();
	if (result.gaps != 0)
		spdlog::warn("{} sequence gaps: the replica died mid-run and the "
					 "liquidity it had seeded was withdrawn each time",
					 result.gaps);
	if (result.clock_regressions != 0)
		spdlog::warn("{} event stamps moved market time backwards and were "
					 "refused; the capture's time axis is not monotonic",
					 result.clock_regressions);

	// A fault in the *input*, as distinct from either of the above: the capture
	// stopped producing before it ran out. Reported after the report rather
	// than instead of it - the frames that did replay are still a result, and
	// what an operator wants next is the line number to go and look at.
	if (!replayed.is_clean()) {
		spdlog::error("the capture did not replay to the end: {}",
					  replayed.stop);
		return EXIT_FAILURE;
	}

	// A fault in the harness or the configuration, as distinct from a bad
	// result: the numbers above describe less work than was asked for.
	if (result.misroutes != 0 || result.commands_dropped != 0 ||
		result.rounds_exhausted != 0 || result.dropped_levels != 0) {
		spdlog::error("the run is not a clean replay: {} misroutes, {} dropped "
					  "commands, {} events that never settled, {} level "
					  "changes the listing's spec could not express",
					  result.misroutes,
					  result.commands_dropped,
					  result.rounds_exhausted,
					  result.dropped_levels);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

} // namespace exchange::app
