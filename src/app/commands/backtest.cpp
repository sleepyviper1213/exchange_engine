#include "backtest.hpp"

#include "core/logging.hpp"
#include "core/util/slurp.hpp"
#include "market-data/format.hpp" // IWYU pragma: keep — fmt::formatter<depth_parse_error>
#include "market_data.hpp"
#include "strategy/backtest.hpp"
#include "strategy/backtest/format.hpp" // IWYU pragma: keep — fmt::formatter<report_summary>
#include "trading-engine.hpp"

#include <fmt/std.h>
#include <spdlog/stopwatch.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;

namespace exchange::app {
namespace {

/// @brief Parse a tick or lot size from decimal text onto @p scale.
/// @return The scaled increment, or nothing — the reason is logged.
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
 * @brief Drive @p run over @p feed with @p actor, reporting progress.
 *
 * Templated on the trader for the same reason @c session's own hooks are: an
 * absent @c on_market compiles away rather than becoming a branch, and the
 * quoter's hooks stay direct calls. @see backtest::session
 */
template <exchange::strategy::backtest::trader Trader>
void drive_backtest(exchange::strategy::backtest::session &run,
					const market_data::book_snapshot &seed,
					const std::vector<market_data::binance::DepthUpdate> &feed,
					std::uint64_t limit, Trader &actor) {
	// The seed goes in as a copy: on_snapshot consumes it, and a caller may
	// legitimately want to re-seed from the same payload after a gap.
	if (!run.on_snapshot(market_data::book_snapshot{seed}, actor))
		spdlog::warn("the seed snapshot did not bring the replica live; it may "
					 "predate the capture");

	std::uint64_t applied = 0;
	for (const auto &update : feed) {
		if (limit != 0 && applied >= limit) break;
		run.on_event(market_data::binance::normalise(update), actor);
		++applied;
	}
	run.finish(actor);
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

	const auto seed_json =
		binance::parse_binance_depth(slurp(settings.snapshot),
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
	// tick grid — reference data is a deployment fact, so a misaligned one is a
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
	const auto feed =
		binance::parse_binance_depth_updates(jsonl,
											 settings.price_decimals,
											 settings.qty_decimals);
	if (!feed) {
		spdlog::error("capture parse failed for {}: {}",
					  settings.file,
					  feed.error());
		return EXIT_FAILURE;
	}
	spdlog::info("backtesting {} over {} frames from {} (tick {}, lot {})",
				 settings.symbol,
				 feed->size(),
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
		drive_backtest(run,
					   binance::normalise(*seed_json),
					   *feed,
					   settings.events,
					   quoter);
		spdlog::info("quoter placed {} quotes, {} commands accepted, {} stalls",
					 quoter.quotes(),
					 quoter.submitted(),
					 quoter.stalls());
		// A run that quoted nothing is the one outcome the report above cannot
		// explain — every counter in it is zero either way. These are the only
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
		drive_backtest(run,
					   binance::normalise(*seed_json),
					   *feed,
					   settings.events,
					   idle);
	}

	spdlog::info("replayed in {:.3f}s of wall clock", watch.elapsed().count());

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
