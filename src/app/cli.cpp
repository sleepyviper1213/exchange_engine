#include "cli.hpp"

#include "strategy/backtest.hpp"
#include "strategy/backtest/format.hpp" // IWYU pragma: keep — fmt::formatter<report_summary>
#include "core/concurrency/affinity.hpp"
#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep — fmt::formatter<topology>
#include "core/logging.hpp"
#include "core/metrics.hpp"
#include "core/metrics/format.hpp" // IWYU pragma: keep — fmt::formatter<registry>, <histogram::snapshot>
#include "core/util/slurp.hpp"
#include "market-data/format.hpp" // IWYU pragma: keep — fmt::formatter<book_ladder>
#include "market_data.hpp"
#include "trading-engine.hpp"
#include "trading-engine/format.hpp" // IWYU pragma: keep — fmt::formatter<order_book>, <order_manager>
#include "transport.hpp"

#include <CLI/CLI.hpp>
#include <fmt/std.h>
#include <spdlog/stopwatch.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>


using namespace exchange::engine;
using namespace exchange::engine::orders;

// Per-command drivers — thin wrappers over transport (I/O) and the engine.
// File-internal: the CLI is the only caller (see run_cli below).
//
// The split here is between a command's RESULT and its COMMENTARY. A book
// ladder, a snapshot, a throughput figure go to stdout through fmt::println,
// unadorned, because something downstream may be reading them — timestamping
// those would corrupt data, not annotate it. Everything else — what was
// fetched, how long it took, what went wrong — goes to the log, which is stderr
// plus the file (see core/logging.hpp).
namespace exchange::app {
int cmd_snapshot(const std::string &symbol, const std::string &file, int limit,
				 int price_decimals, int qty_decimals) {
	namespace binance = market_data::binance;

	const auto begin = std::chrono::system_clock::now();
	std::expected<std::string, std::string> json = std::unexpected("uninit");

	if (!file.empty()) {
		spdlog::info("loading depth snapshot from {}", file);
		json = exchange::core::util::slurp(file);
		if (json->empty())
			json = std::unexpected(fmt::format("cannot read {}", file));
	} else {
		auto [host, target] = binance::depth_snapshot(symbol, limit);
		spdlog::info("fetching depth snapshot {} limit={} from {}",
					 symbol,
					 limit,
					 host);
		spdlog::debug("GET {}{}", host, target);
		json =
			exchange::transport::rest::get(std::move(host), std::move(target));
	}

	if (!json) {
		spdlog::error("snapshot fetch failed: {}", json.error());
		return EXIT_FAILURE;
	}
	spdlog::debug("snapshot payload {} bytes", json->size());

	const auto snapshot =
		binance::parse_binance_depth(*json, price_decimals, qty_decimals);
	if (!snapshot) {
		spdlog::error("snapshot parse failed: {}", snapshot.error());
		return EXIT_FAILURE;
	}

	// A REST snapshot is published depth, so it reconstructs into an l2_book —
	// resting anonymous orders in a matching engine would model a queue the
	// payload says nothing about.
	market_data::l2_book book;
	for (const auto &[price, qty] : snapshot->bids)
		book.set_level(side_t::bid, price, qty);
	for (const auto &[price, qty] : snapshot->asks)
		book.set_level(side_t::ask, price, qty);
	const auto end = std::chrono::system_clock::now();
	spdlog::info("snapshot ready in {}: {}", end - begin, *snapshot);

	// The result, on stdout, untimestamped: this is what a caller redirecting
	// stdout is asking for. book_ladder rather than the book directly, because
	// the command was given the symbol's tick and step and they are the only
	// thing that turns the book's scaled integers back into quoted prices.
	fmt::println("{}",
				 market_data::book_ladder{&book, price_decimals, qty_decimals});
	return EXIT_SUCCESS;
}

// Capture the venue's published diff-depth feed: the `<symbol>@depth` stream
// whose frames carry absolute aggregate sizes per price. market-data decides
// which endpoint that is; transport just records the frames.
int cmd_capture(const std::string &symbol, const std::string &outfile,
				int seconds, std::string_view speed) {
	namespace binance = market_data::binance;

	if (seconds <= 0) {
		spdlog::error("seconds must be positive (got {})", seconds);
		return EXIT_FAILURE;
	}

	auto [host, port, target] = binance::diff_depth_stream(
		symbol,
		speed == "1000ms" ? binance::depth_speed::every_1000ms
						  : binance::depth_speed::every_100ms);

	spdlog::info("capturing {} @{} for {}s from {} -> {}",
				 symbol,
				 speed,
				 seconds,
				 host,
				 outfile);

	const auto result =
		exchange::transport::ws::capture(std::move(host),
										 std::move(port),
										 std::move(target),
										 outfile,
										 std::chrono::seconds(seconds));
	if (!result) {
		spdlog::error("capture failed: {}", result.error());
		return EXIT_FAILURE;
	}
	spdlog::info("capture complete: {}", outfile);
	return EXIT_SUCCESS;
}

int cmd_demo(std::uint64_t num_orders,
			 const core::metrics::settings &metrics_settings) {
	namespace affinity = core::concurrency::affinity;
	namespace metrics  = core::metrics;

	constexpr price_t MID =
		10000; // reference price_t the synthetic flow orbits
	if (num_orders == 0) {
		spdlog::error("num_orders must be positive");
		return EXIT_FAILURE;
	}

	// Place the producer (this thread) and consumer on dedicated cores, each on
	// its own physical core where the topology allows — the SPSC hand-off pays
	// real cross-core coherency traffic instead of thrashing one core's L1/L2.
	affinity::core_allocator cores(affinity::discover());
	const auto producer_core = cores.reserve("producer");
	const auto consumer_core = cores.reserve("consumer");
	const auto core_str      = [](std::optional<affinity::core_id> c) {
		return c ? fmt::to_string(*c) : std::string("any");
	};
	// Pinning is a property of the run, not a result of it: an unpinned pair
	// measures the scheduler, so which cores were reserved has to be
	// recoverable from the log when a throughput figure later looks wrong.
	spdlog::info("{}  (producer->cpu {}, consumer->cpu {})",
				 cores.get_topology(),
				 core_str(producer_core),
				 core_str(consumer_core));
	if (!producer_core || !consumer_core)
		spdlog::warn("a core reservation was refused; the hand-off may share a "
					 "core and the throughput below is not comparable");

	// One listing, so the routing this demo exercises is trivial — but the
	// commands still have to name it, because a partition refuses a symbol it
	// was not given rather than inventing a book for it.
	constexpr symbol_id_t SYMBOL = 0;

	// Self-cancelling crossing pairs: an ASK rests at a price, then a BID at
	// the same price and size consumes it whole. Sides still alternate and the
	// price still sweeps +/-5 ticks around the mid, so the run exercises match,
	// rest, pop_front and level insert/erase at varied sorted positions — but
	// the book returns to empty after every pair.
	//
	// That last property is the point, and it used to be missing. Pairing each
	// order with an opposite one only *near* it in price left the extremes
	// uncrossed, so resting liquidity accumulated without bound; the book
	// absorbed it (its node pool chains another block) but order_manager will
	// not, because refusing an order beats forgetting a live one. The run
	// therefore filled the record store at ~186k orders and rejected every
	// command after that, reporting a throughput figure that was really
	// measuring how fast the engine can say no. benchmark/matching_engine.cpp
	// generates its flow this way for the same reason.
	const auto make_order = [MID](std::uint64_t i) noexcept {
		// Locals are deliberately not named after their types: inside a scope
		// that declares a `price`, `static_cast<price>` resolves to the
		// variable rather than the type and stops compiling.
		const std::uint64_t pair = i / 2U;
		const side_t s           = (i & 1U) ? side_t::bid : side_t::ask;
		const price_t px         = MID + static_cast<price_t>(pair % 11U) - 5U;
		const quantity_t qty     = 1 + static_cast<quantity_t>(pair % 5U);
		return event::command::place(order{.id        = i + 1U,
										   .symbol_id = SYMBOL,
										   .side      = s,
										   .price     = px,
										   .qty       = qty});
	};

	std::atomic<std::uint64_t> trade_count{0};
	std::atomic<std::int64_t> matched_volume{0};
	std::atomic<std::uint64_t> reject_count{0};
	// The reason of the first refusal, kept so the summary can name it. One
	// cause explains a whole run's worth of rejections here — the interesting
	// question is never "which of these many reasons" but "why did it start".
	std::atomic<reject_reason> first_reject{reject_reason::NONE};

	// Declared unconditionally (it is four cache lines on the stack, nothing
	// more) but only wired into the partition — and so only ever written to —
	// when the operator asked for it. See core/metrics/settings.hpp: metrics
	// are off by default, and a caller that never mentions --metrics-enabled
	// gets exactly the cost of an unmetered partition.
	execution::partition_metrics engine_metrics{
		.drain_latency_ns{metrics::latency_budgets{
			.p99_ns  = metrics_settings.drain_p99_budget_ns,
			.p999_ns = metrics_settings.drain_p999_budget_ns,
			.max_ns  = metrics_settings.drain_max_budget_ns,
		}},
	};

	// Watches drain_latency_ns on metrics_settings.interval_ms for the whole
	// run rather than only at the end — see core/metrics/sla_monitor.hpp.
	// std::optional so it is constructed only when metrics were asked for,
	// and reset() right after the run so the monitor's thread is not still
	// polling a histogram this function is about to let go out of scope.
	std::optional<metrics::sla_monitor> drain_monitor;
	if (metrics_settings.enabled)
		drain_monitor.emplace(
			engine_metrics.drain_latency_ns,
			std::chrono::milliseconds(metrics_settings.interval_ms),
			[](const metrics::histogram &h) {
				spdlog::warn("drain latency breached its budget: {}", h.read());
			});

	execution::engine_partition<1024> engine(
		[&](const std::vector<trade> &batch) noexcept {
			std::int64_t v = 0;
			for (const trade &t : batch) v += t.volume;
			trade_count.fetch_add(batch.size(), std::memory_order_relaxed);
			matched_volume.fetch_add(v, std::memory_order_relaxed);
		},
		// An outcome sink, and not decoration: without one a run in which the
		// engine refused every order looks exactly like a fast one. It reports
		// fewer trades and a *higher* orders/s, because saying no is cheaper
		// than matching. That is the most misleading way for a benchmark to
		// fail, so the refusals are counted and printed.
		[&](const std::vector<order_outcome> &batch) noexcept {
			std::uint64_t refused = 0;
			for (const order_outcome &o : batch) {
				if (o.type != OutcomeType::REJECTED) continue;
				++refused;
				reject_reason none = reject_reason::NONE;
				// Relaxed: only the first writer matters and nothing is ordered
				// against it — the value is read after both threads have
				// joined.
				first_reject.compare_exchange_strong(none,
													 o.reason,
													 std::memory_order_relaxed,
													 std::memory_order_relaxed);
			}
			if (refused != 0)
				reject_count.fetch_add(refused, std::memory_order_relaxed);
		},
		execution::book_manager::DEFAULT_BOOK_CAPACITY,
		execution::order_manager::DEFAULT_CAPACITY,
		metrics_settings.enabled ? &engine_metrics : nullptr);
	// On the consumer's side of the contract, and before the producer starts.
	engine.listing(SYMBOL);

	const spdlog::stopwatch watch;

	// Consumer: drain until every submitted command has been applied.
	std::thread consumer([&] {
		// pin_this_thread_to has already logged which syscall refused and on
		// what core. What it cannot know is what that costs *here*, which is
		// the only thing worth adding: an unpinned consumer makes the figure
		// below a measurement of the scheduler as much as of the engine.
		if (!cores.pin_this_thread_to("consumer"))
			spdlog::warn("consumer is unpinned; the throughput below is not "
						 "comparable with a pinned run");
		std::uint64_t applied = 0;
		while (applied < num_orders) {
			const std::size_t n = engine.drain_and_flush();
			if (n == 0) std::this_thread::yield();
			else applied += n;
		}
	});

	// Producer: this thread. Retries on a full lockfree (lossless
	// back-pressure).
	if (!cores.pin_this_thread_to("producer"))
		spdlog::warn("producer is unpinned; the throughput below is not "
					 "comparable with a pinned run");
	for (std::uint64_t i = 0; i < num_orders; ++i) {
		const event::command cmd = make_order(i);
		while (!engine.submit(cmd)) std::this_thread::yield();
	}

	consumer.join();

	const double secs = watch.elapsed().count();

	fmt::println("submitted {} orders in {:.3f}s  ({:.2f}M orders/s)",
				 num_orders,
				 secs,
				 static_cast<double>(num_orders) / secs / 1e6);
	fmt::println("trades: {}   matched qty: {}",
				 trade_count.load(),
				 matched_volume.load());
	fmt::println("resting {}", *engine.book(SYMBOL));

	// The record store's own reading, printed every run rather than only on
	// trouble: peak against capacity is the number the sizing has to be argued
	// from, and this is the only place it can be read.
	fmt::println("{}", engine.orders());

	// Reference wiring for core/metrics: name the fields recorded above, print
	// the drain-latency distribution the way docs/performance.md asks any
	// latency budget be read (a percentile, not a mean), and — if the operator
	// asked for it — overwrite the exposition file a scrape-based collector
	// would tail. drain_monitor already watched this continuously while the
	// run was in flight; this is the final read after it stopped, covering
	// whatever happened between its last tick and the run ending.
	if (metrics_settings.enabled) {
		metrics::registry registry;
		registry.add("engine_commands_processed",
					 engine_metrics.commands_processed);
		registry.add("engine_trades_emitted", engine_metrics.trades_emitted);
		registry.add("engine_misroutes", engine_metrics.misroutes);
		registry.add("engine_drain_latency_ns",
					 engine_metrics.drain_latency_ns);

		fmt::println("drain latency: {}",
					 engine_metrics.drain_latency_ns.read());

		// One last, synchronous check for the gap between drain_monitor's
		// last periodic tick and now, reusing its own callback instead of
		// hand-rolling the same is_healthy()-then-warn a second time — see
		// core/metrics/sla_monitor.hpp::check_now().
		drain_monitor->check_now();
		// Stop watching now that the run is over and this function is about
		// to let drain_latency_ns' owner (engine_metrics) go out of scope.
		drain_monitor.reset();

		std::ofstream out(metrics_settings.output_file, std::ios::trunc);
		if (!out) {
			spdlog::error("could not open metrics file {}",
						  metrics_settings.output_file);
		} else {
			out << fmt::format("{}", registry);
			spdlog::info("wrote metrics to {}", metrics_settings.output_file);
		}
	}

	const std::uint64_t refused = reject_count.load();
	if (refused == 0) return EXIT_SUCCESS;

	// A refused order never reached a book, so it is missing from the trade
	// count and from the orders/s above — both of which are then measuring a
	// smaller run than the one that was asked for. Loud, and a failure exit:
	// a throughput figure taken from a partial run is worse than none.
	spdlog::error(
		"{} of {} orders were refused ({}); the figures above describe "
		"the {} that were not",
		refused,
		num_orders,
		describe(first_reject.load()),
		num_orders - refused);
	return EXIT_FAILURE;
}

// --- replay: rebuild the venue's published depth from a JSONL diff capture ---
// The managed-local-order-book procedure end to end: seed from a REST snapshot,
// then stream diffs. The target is market data's l2_book throughout — the
// matching engine is not involved, because none of this is our order flow.
// @param snapshot_file  Non-empty to seed the book from a saved REST snapshot.
int cmd_replay(const std::string &file, const std::string &snapshot_file,
			   int price_decimals, int qty_decimals) {
	using exchange::core::util::slurp;
	namespace binance = market_data::binance;

	market_data::l2_book book;

	// Optional seed: absolute levels from a saved REST snapshot.
	if (!snapshot_file.empty()) {
		const auto snap = binance::parse_binance_depth(slurp(snapshot_file),
													   price_decimals,
													   qty_decimals);
		if (!snap) {
			spdlog::error("snapshot parse failed for {}: {}",
						  snapshot_file,
						  snap.error());
			return EXIT_FAILURE;
		}
		for (const auto &[price, qty] : snap->bids)
			book.set_level(side_t::bid, price, qty);
		for (const auto &[price, qty] : snap->asks)
			book.set_level(side_t::ask, price, qty);
		spdlog::info("seeded from {}: {}", snapshot_file, *snap);
	} else {
		// Worth saying plainly: with no seed the book only ever holds the
		// prices the capture happened to touch, so its depth is an artefact of
		// the recording rather than the venue's published book.
		spdlog::warn("no --snapshot seed; the replayed book will be partial");
	}

	// Read + parse the JSONL feed (one depthUpdate frame per line).
	const std::string jsonl = slurp(file);
	if (jsonl.empty()) {
		spdlog::error("cannot read {} (missing or empty)", file);
		return EXIT_FAILURE;
	}
	spdlog::debug("read {} bytes from {}", jsonl.size(), file);
	const auto updates = binance::parse_binance_depth_updates(jsonl,
															  price_decimals,
															  qty_decimals);
	if (!updates) {
		spdlog::error("replay parse failed for {}: {}", file, updates.error());
		return EXIT_FAILURE;
	}

	// Apply every update to the book, timing the hot loop.
	const auto start   = std::chrono::steady_clock::now();
	std::size_t levels = 0;
	for (const auto &update : *updates) {
		binance::apply_depth_update(book, update);
		levels += update.bids.size() + update.asks.size();
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	const double secs  = std::chrono::duration<double>(elapsed).count();

	spdlog::info("replayed {} updates ({} level changes) from {} in {:.3f}s",
				 updates->size(),
				 levels,
				 file,
				 secs);
	fmt::println("{}",
				 market_data::book_ladder{.book           = &book,
										  .price_decimals = price_decimals,
										  .qty_decimals   = qty_decimals});
	return EXIT_SUCCESS;
}

// --- backtest: the same capture, but through the whole engine ---------------
//
// `replay` above reconstructs the venue's published depth and stops there —
// market-data only, no matching, no orders. This runs the *rest* of the system
// over the same file: the depth becomes resting liquidity in a real order_book,
// a trader quotes into a real risk gate, the matching engine executes what
// crosses, and a fill model supplies the one thing the recording cannot (see
// strategy/backtest/fill_model.hpp). The output is a report, not a book.

/// @brief Everything `backtest` was asked for, gathered so the driver's
///        signature stays readable. @see add_backtest
struct backtest_settings {
	std::string file;           ///< JSONL capture of depthUpdate frames
	std::string snapshot;       ///< REST depth JSON to seed the replica
	std::string symbol = "BACKTEST"; ///< display name for the listing
	std::string tick   = "0.01";     ///< price increment, as decimal text
	std::string lot    = "0.01";     ///< size increment, as decimal text
	int price_decimals = 2;
	int qty_decimals   = 2;
	std::uint64_t events = 0; ///< stop after this many frames; 0 is all of them
	std::int64_t max_position = 0; ///< lots; 0 leaves the position limit open
	int improve_ticks         = 1; ///< how far inside the touch to quote
	int lots                  = 1; ///< quote size, in lots
	int requote_ms            = 0; ///< market time a quote is left standing
	bool fill_on_lock         = false; ///< fill on a locked market, not only a
									   ///< trade-through
	bool quote                = true;  ///< run the reference quoter at all
};

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

int cmd_backtest(const backtest_settings &settings) {
	namespace binance  = market_data::binance;
	namespace backtest = exchange::strategy::backtest;
	using exchange::core::util::slurp;

	// --- reference data, before anything is parsed against it ---------------
	const auto tick_scaled =
		increment(settings.tick, settings.price_decimals, "tick");
	const auto lot_scaled = increment(settings.lot, settings.qty_decimals, "lot");
	if (!tick_scaled || !lot_scaled) return EXIT_FAILURE;

	const auto seed_json = binance::parse_binance_depth(slurp(settings.snapshot),
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
	const auto feed = binance::parse_binance_depth_updates(jsonl,
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
					static_cast<std::uint64_t>(settings.requote_ms) * 1'000'000U,
			});
		drive_backtest(run, binance::normalise(*seed_json), *feed,
					   settings.events, quoter);
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
		// No order flow at all: this replays the capture through the bridge, the
		// partition and the matching engine and checks the harness rather than a
		// strategy. Every fill counter must come back zero.
		backtest::null_trader idle;
		drive_backtest(run, binance::normalise(*seed_json), *feed,
					   settings.events, idle);
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

// Each registrar binds CLI11 options to storage that must outlive the call —
// parsing runs later, back in main() — so the option variables are function
// static. The CLI is built and parsed exactly once, so that is safe.
void add_snapshot(CLI::App &app, int &rc) {
	auto *snap = app.add_subcommand(
		"snapshot",
		"Fetch (or load) a Binance depth snapshot and print top of book");
	std::string symbol, file;
	int limit          = 100;
	int price_decimals = 2;
	int qty_decimals   = 2;
	snap->add_option("symbol", symbol, "Binance symbol, e.g. SOLUSDT");
	snap->add_option("--file",
					 file,
					 "Load a saved depth JSON or fetching live");
	snap->add_option("--limit", limit, "REST depth limit")
		->capture_default_str();
	snap->add_option("--price-decimals", price_decimals, "Tick precision")
		->capture_default_str();
	snap->add_option("--qty-decimals", qty_decimals, "Step precision")
		->capture_default_str();
	snap->callback([&] {
		if (symbol.empty() && file.empty())
			throw CLI::ValidationError("snapshot", "provide SYMBOL or --file");
		rc = cmd_snapshot(symbol, file, limit, price_decimals, qty_decimals);
	});
}

void add_capture(CLI::App &app, int &rc) {
	auto *cap = app.add_subcommand(
		"capture",
		"Stream a Binance diff-depth WebSocket to a JSONL file");
	std::string symbol;
	std::string outfile;
	std::string speed = "100ms";
	int seconds       = 30;
	cap->add_option("symbol", symbol, "Binance symbol")->required();
	cap->add_option("outfile", outfile, "Destination JSONL file")->required();
	cap->add_option("--seconds", seconds, "Recording duration (seconds)")
		->capture_default_str();
	cap->add_option("--speed", speed, "Update cadence")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	cap->callback([&] { rc = cmd_capture(symbol, outfile, seconds, speed); });
}

void add_demo(CLI::App &app, int &rc,
			  const core::metrics::settings &metrics_settings) {
	auto *demo = app.add_subcommand(
		"demo",
		"Run the MatchingEngine end-to-end over the SPSC queue");
	static std::uint64_t num_orders = 2'000'000;
	demo->add_option("num_orders", num_orders, "Synthetic orders to submit")
		->capture_default_str();
	demo->callback([&rc, &metrics_settings] {
		rc = cmd_demo(num_orders, metrics_settings);
	});
}

void add_replay(CLI::App &app, int &rc) {
	auto *replay = app.add_subcommand(
		"replay",
		"Replay a JSONL diff-depth capture through an OrderBook");
	std::string file;
	std::string snapshot;
	int price_decimals = 2;
	int qty_decimals   = 2;
	replay->add_option("file", file, "JSONL capture of depthUpdate frames")
		->required()
		->check(CLI::ExistingFile);
	replay
		->add_option("--snapshot",
					 snapshot,
					 "Seed the book from a saved REST depth JSON")
		->check(CLI::ExistingFile);
	replay->add_option("--price-decimals", price_decimals, "Tick precision")
		->capture_default_str();
	replay->add_option("--qty-decimals", qty_decimals, "Step precision")
		->capture_default_str();
	replay->callback(
		[&] { rc = cmd_replay(file, snapshot, price_decimals, qty_decimals); });
}

void add_backtest(CLI::App &app, int &rc) {
	auto *bt = app.add_subcommand(
		"backtest",
		"Run a strategy against a recorded capture through the whole engine");
	// Static for the same reason the other registrars' locals are not: this one
	// is a single aggregate, so one static keeps every option's storage alive
	// until CLI11 parses, instead of a dozen.
	static backtest_settings settings;

	bt->add_option("file", settings.file, "JSONL capture of depthUpdate frames")
		->required()
		->check(CLI::ExistingFile);
	bt->add_option("--snapshot",
				   settings.snapshot,
				   "REST depth JSON seeding the replica. Required: without a "
				   "seed the book only holds prices the capture happened to "
				   "touch, and nothing meaningful can fill against it")
		->required()
		->check(CLI::ExistingFile);
	bt->add_option("--symbol", settings.symbol, "Display name for the listing")
		->capture_default_str();
	bt->add_option("--price-decimals", settings.price_decimals, "Price scale")
		->capture_default_str();
	bt->add_option("--qty-decimals", settings.qty_decimals, "Quantity scale")
		->capture_default_str();
	bt->add_option("--tick",
				   settings.tick,
				   "Price increment, e.g. 0.01. Must be exact at --price-decimals")
		->capture_default_str();
	bt->add_option("--lot",
				   settings.lot,
				   "Size increment, e.g. 0.01. Must be exact at --qty-decimals")
		->capture_default_str();
	bt->add_option("--events",
				   settings.events,
				   "Stop after this many frames (0 = the whole file)")
		->capture_default_str();
	bt->add_option("--improve",
				   settings.improve_ticks,
				   "Ticks inside the venue's touch to quote. Below 1 the quote "
				   "can never be traded through and will never fill passively")
		->capture_default_str();
	bt->add_option("--lots", settings.lots, "Quote size, in lots")
		->capture_default_str();
	bt->add_option("--requote-ms",
				   settings.requote_ms,
				   "Market time a quote is left standing before it is replaced. "
				   "Zero requotes on every frame, which is also why zero fills "
				   "nothing: the quoter steps out of the way before the market "
				   "ever reaches it, and being slower is what creates exposure")
		->capture_default_str();
	bt->add_option("--max-position",
				   settings.max_position,
				   "Risk limit on absolute net position, in lots (0 = open)")
		->capture_default_str();
	bt->add_flag("--fill-on-lock",
				 settings.fill_on_lock,
				 "Fill a resting order when the venue quotes *at* its price, "
				 "not only when it trades through. Strictly more optimistic");
	bt->add_flag("--no-quote{false}",
				 settings.quote,
				 "Replay with no order flow — exercises the harness, not a "
				 "strategy; every fill counter must come back zero");

	bt->callback([&rc] { rc = cmd_backtest(settings); });
}
} // namespace exchange::app