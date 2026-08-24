#include "serve.hpp"

#include "app/wall_clock.hpp"
#include "session/live_feed.hpp"
#include "session/live_session.hpp"
#include "session/gate_logger.hpp"
#include "session/engine_logger.hpp"
#include "core/concurrency/affinity.hpp"
#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/logging.hpp"
#include "core/util/owned_file.hpp"
#include "core/metrics.hpp"
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "market-data/binance/depth_speed.hpp"
#include "market-data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "event/lifecycle/lifecycle.hpp"
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "symbol/symbol_spec.hpp"
#include "symbol/validation.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/std.h> // IWYU pragma: keep - fmt::formatter<std::filesystem::path>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace exchange::engine;

namespace exchange::app {

// The live pipeline this command drives, named by its owning namespace rather
// than pulled in wholesale: `session` is a module now, and a using-directive
// would hide which of these names came from it. @see src/session/
using session::engine_logger;
using session::gate_logger;
using session::live_feed_options;
using session::live_feed_report;
using session::live_handler;
using session::live_session;
using session::live_session_options;
using session::live_session_report;
using session::run_live_feed;
namespace {

namespace asio = boost::asio;

/// @brief @p ms as a count of nanoseconds, for the risk module's fields.
[[nodiscard]] constexpr std::uint64_t
to_ns(std::chrono::milliseconds ms) noexcept {
	return static_cast<std::uint64_t>(std::chrono::nanoseconds{ms}.count());
}

/// @brief @p ns as whole milliseconds, for a log line an operator reads.
///        Truncating, which is the right way round for a width: a 1'048'576 ns
///        window reads as 1 ms rather than as 2.
[[nodiscard]] constexpr std::chrono::milliseconds
to_ms(std::uint64_t ns) noexcept {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::nanoseconds{ns});
}

/// @brief The session this command runs: the live topology, with the two log
///        taps the modules below cannot install for themselves. @see
///        session/gate_logger.hpp
using serving_session =
	live_session<risk::steady_nanos, gate_logger, engine_logger>;

/**
 * @brief The shift naming a window of at least @p ms milliseconds.
 *
 * Rounded up, so a window is never narrower than the one asked for: a
 * surveillance threshold quietly checked over half the interval an operator
 * named is worse than one checked over twice it. @c fixed_window clamps the
 * result, so an absurd request gets the widest supported window rather than
 * undefined behaviour.
 */
[[nodiscard]] unsigned window_log2_for(std::chrono::milliseconds ms) noexcept {
	const std::uint64_t ns = to_ns(ms);
	if (ns <= 1) return 0;
	return static_cast<unsigned>(std::bit_width(ns - 1));
}

/// @brief Parse a tick or lot size from decimal text onto @p scale.
[[nodiscard]] std::optional<std::int64_t>
increment(std::string_view text, int scale, std::string_view what) {
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


/// @brief Overwrite @p file with @p registry's exposition. Failure is logged
///        and swallowed: this is the least important thing the command does and
///        it must not take a running session down with it.
void write_exposition(const core::metrics::registry &registry,
					  const std::filesystem::path &file) {
	const core::util::owned_file out = core::util::open_shared(file, "wb");
	if (out == nullptr) {
		spdlog::error("could not open metrics file {}", file);
		return;
	}
	fmt::print(out.get(), "{}", registry);
}

/**
 * @brief Re-render the exposition file every @p every until cancelled.
 *
 * The loop `metrics-interval-ms` has always named and nothing has run: a
 * one-shot command has no "periodic" to be, and this is the first command that
 * stays up long enough to have one. Cancellation arrives as an error code from
 * the timer when the io_context stops, which is the signal to stop writing -
 * the final render happens on the way out of @c cmd_serve, where the session is
 * quiescent and the numbers are the run's rather than a snapshot mid-frame.
 */
asio::awaitable<void> publish_metrics(std::chrono::milliseconds every,
									  const core::metrics::registry *registry,
									  std::filesystem::path file) {
	asio::steady_timer timer(co_await asio::this_coro::executor);
	for (;;) {
		timer.expires_after(every);
		auto [error] = co_await timer.async_wait(session::detail::kToken);
		if (error) co_return;
		write_exposition(*registry, file);
	}
}

/**
 * @brief Ask the venue for @p settings' listing grid.
 *
 * @return What the venue published, or nothing if it could not be asked - in
 *         which case the caller falls back to the flags and says so. A failure
 *         here is deliberately not fatal: the grid is an improvement on a guess,
 *         not a precondition for running, and a research tool that refuses to
 *         start because a REST endpoint blinked is worse than one that starts on
 *         a stated assumption.
 *
 * Blocking, and that is safe precisely here: this runs before the io_context
 * exists, so there is no loop to stall. It is also the last moment it *can* run,
 * because the listing it produces is what every subsequent number is quantised
 * against. @see make_listing
 */
[[nodiscard]] std::optional<market_data::binance::symbol_filters>
fetch_venue_grid(const serve_settings &settings) {
	namespace binance = market_data::binance;

	auto [host, target] = binance::exchange_info(settings.symbol);
	spdlog::debug("GET {}{}", host, target);
	const auto body = transport::rest::get(std::move(host), std::move(target));
	if (!body) {
		spdlog::warn("could not read {}'s grid from the venue: {}",
					 settings.symbol,
					 binance::describe_api_error(body.error().body,
												 body.error().message()));
		return std::nullopt;
	}

	auto filters = binance::parse_exchange_info(*body, settings.symbol);
	if (!filters) {
		spdlog::warn("could not read {}'s grid from the venue: {}",
					 settings.symbol,
					 filters.error());
		return std::nullopt;
	}

	// Logged whether or not it differs, because "the venue says the same thing"
	// is the reassurance an operator wants and costs one line. A difference is
	// escalated, since it means every size in a run configured the old way was
	// quantised on the wrong grid.
	spdlog::info("venue grid for {}: tick {} ({} dp), step {} ({} dp), status {}",
				 filters->symbol,
				 filters->tick_size,
				 filters->price_decimals,
				 filters->step_size,
				 filters->qty_decimals,
				 filters->status);
	if (filters->qty_decimals != settings.qty_decimals)
		spdlog::warn("--qty-decimals {} would have truncated sizes the venue "
					 "publishes to {} decimals; using the venue's",
					 settings.qty_decimals,
					 filters->qty_decimals);
	if (!filters->is_trading())
		spdlog::warn("{} is {} rather than TRADING - the feed may be idle",
					 filters->symbol,
					 filters->status);
	return *filters;
}

/**
 * @brief Build the listing from what the operator gave, or nothing.
 *
 * Separated from @c cmd_serve because it is the whole of the validation and
 * none of the run: reference data is checked before anything is measured
 * against it, and a bad tick size should fail before a socket is opened. The
 * reason is logged either way.
 */
[[nodiscard]] std::optional<symbol_spec>
make_listing(const serve_settings &settings,
			 const std::optional<market_data::binance::symbol_filters> &venue) {
	// The venue's grid wins where we have it, because it is the grid the numbers
	// on the wire are quantised to and the flags are a guess at it. Precision is
	// the half that matters: surplus decimals are *truncated* on the way in, so a
	// step configured coarser than the venue's silently rounds small levels to
	// nothing. @see binance::symbol_filters
	const std::string tick = venue ? venue->tick_size : settings.tick;
	const std::string lot  = venue ? venue->step_size : settings.lot;
	const int price_decimals =
		venue ? venue->price_decimals : settings.price_decimals;
	const int qty_decimals =
		venue ? venue->qty_decimals : settings.qty_decimals;

	const auto tick_scaled = increment(tick, price_decimals, "tick");
	const auto lot_scaled  = increment(lot, qty_decimals, "lot");
	if (!tick_scaled || !lot_scaled) return std::nullopt;

	// Uncollared, so the anchor is inert - one tick is the smallest on-grid
	// value and is honest about being a placeholder, where a guess at the
	// instrument's price would look like a fact. @see serve_settings::reference
	std::int64_t reference = *tick_scaled;
	if (!settings.reference.empty()) {
		const auto given =
			increment(settings.reference, price_decimals, "reference");
		if (!given) return std::nullopt;
		if (*given % *tick_scaled != 0) {
			spdlog::error("--reference '{}' is not on the tick grid",
						  settings.reference);
			return std::nullopt;
		}
		reference = *given;
	}

	return symbol_spec{0,
					   settings.symbol,
					   price_decimals,
					   qty_decimals,
					   *tick_scaled,
					   *lot_scaled,
					   reference};
}

/// @brief Translate the CLI's feed options, cadence already validated.
[[nodiscard]] live_feed_options
feed_options_from(const serve_settings &settings,
				  market_data::binance::depth_speed cadence) {
	live_feed_options options;
	options.duration        = std::chrono::seconds(settings.seconds);
	options.limit           = settings.limit;
	options.price_decimals  = settings.price_decimals;
	options.qty_decimals    = settings.qty_decimals;
	options.speed           = cadence;
	options.reconnect_delay = std::chrono::milliseconds(settings.reconnect_ms);
	options.max_reconnects  = settings.max_reconnects;
	return options;
}

/**
 * @brief The matching thread's whole life.
 *
 * Owns exactly one member of the session - @c drain_and_publish - which is the
 * whole threading contract. @see live_session.hpp
 *
 * @param run The session. Only its consumer-side member is touched here.
 * @param stopping Set by the feed thread once nothing more will be submitted.
 * @param cores The reservation table, for pinning.
 */
void match_until_stopped(serving_session &run,
						 const std::atomic<bool> &stopping,
						 core::concurrency::affinity::core_allocator &cores) {
	if (!cores.pin_this_thread_to("matching"))
		spdlog::warn("the matching thread is unpinned; its latency figures are "
					 "not comparable with a pinned run");

	while (!stopping.load(std::memory_order_relaxed))
		if (run.drain_and_publish() == 0) std::this_thread::yield();

	// The producer has stopped submitting, but what it submitted last may not
	// have been matched yet - and what was matched may not have been published.
	// Draining to empty here is what makes the report describe the session
	// rather than describing where this thread happened to be.
	while (run.drain_and_publish() != 0) {}
}

/// @brief Translate the CLI's numbers into the session's policy objects.
[[nodiscard]] live_session_options
policy_from(const serve_settings &settings,
			execution::partition_metrics *metrics) {
	live_session_options options;

	options.quoting.improve_ticks =
		static_cast<price_t>(settings.improve_ticks);
	options.quoting.lots = static_cast<quantity_t>(settings.lots);
	options.quoting.requote_interval_ns =
		to_ns(std::chrono::milliseconds{settings.requote_ms});
	options.quoting.take_liquidity = settings.take;

	// Both knobs read the flag's *negation*: the model's own defaults are the
	// conservative ones, and a switch a user has to set in order to be flattered
	// is the right way round for a number anybody will act on.
	options.simulate_fills             = settings.simulate_fills;
	options.fills.require_trade_through = !settings.fill_on_lock;
	options.fills.model_queue_position  = !settings.front_of_queue;

	if (settings.max_order_qty > 0)
		options.limits.max_order_qty =
			static_cast<quantity_t>(settings.max_order_qty);
	if (settings.max_position > 0)
		options.limits.max_position_lots = settings.max_position;
	options.limits.price_band_bps = settings.price_band_bps;
	options.limits.max_loss       = settings.max_loss;
	options.breaches_to_trip      = settings.breaches_to_trip;

	options.surveillance.max_messages_per_execution =
		settings.max_messages_per_execution;
	options.surveillance.max_executions_per_window =
		settings.max_executions_per_window;
	options.surveillance.max_adverse_run = settings.max_adverse_run;
	if (settings.burst_window_ms != 0)
		options.surveillance.burst_window_log2_ns = window_log2_for(
			std::chrono::milliseconds{settings.burst_window_ms});
	if (settings.otr_window_ms != 0)
		options.surveillance.ratio_window_log2_ns =
			window_log2_for(std::chrono::milliseconds{settings.otr_window_ms});
	if (settings.min_otr_messages != 0)
		options.surveillance.min_messages_to_judge = settings.min_otr_messages;
	options.surveillance.outcome_timeout_ns =
		to_ns(std::chrono::milliseconds{settings.outcome_timeout_ms});

	options.feed_timeout_ns =
		to_ns(std::chrono::milliseconds{settings.feed_timeout_ms});
	options.metrics = metrics;
	return options;
}

/**
 * @brief One line an operator can watch while the run is up.
 *
 * A two-minute run used to print nothing between "subscribed" and the summary,
 * which is the wrong silence for the only command that stays up: the numbers
 * that matter are all moving, and a reader should not have to interrupt the
 * process to see them.
 *
 * Deltas rather than totals for the two rates, because "is it still working" is
 * answered by what changed since the last line and not by a number that only
 * ever grows. The rest are levels, and are printed as such.
 *
 * @note Runs on the io_context's thread, which is the session's producer thread
 *       - so every counter it reads is one this thread owns. A console polling
 *       from elsewhere would be reading them across a boundary they are not
 *       synchronised for. @see live_session.hpp
 */
asio::awaitable<void> report_progress(std::chrono::milliseconds every,
									  const serving_session *run) {
	asio::steady_timer timer(co_await asio::this_coro::executor);
	spdlog::logger &log =
		core::logging::logger_for(core::logging::channel::feed);
	std::uint64_t last_events = 0;
	std::uint64_t last_fills  = 0;
	for (;;) {
		timer.expires_after(every);
		auto [error] = co_await timer.async_wait(session::detail::kToken);
		if (error) co_return;

		const auto &r              = run->report();
		const std::uint64_t fills  = run->monitor().fills().total_executions();
		const std::uint64_t frames = r.events - last_events;
		const std::uint64_t executed = fills - last_fills;
		last_events                  = r.events;
		last_fills                   = fills;

		log.info("+{} frames, +{} fills | working {}, position {} lots, pnl {} "
				 "tick-lots, refused {}, breaker {}",
				 frames,
				 executed,
				 run->gate().working_orders(),
				 run->positions().net_lots(run->symbol()),
				 run->gate().pnl(),
				 run->gate().refused(),
				 run->breaker().state());
	}
}

/**
 * @brief Warn about a cap that cannot be crossed at this feed's cadence.
 *
 * A rule with an unreachable threshold is worse than one that is switched off,
 * because it reads as protection in the log and in the flags. The arithmetic is
 * simple enough to be worth doing up front rather than leaving an operator to
 * infer it from a zero at the end of a run: this lane's executions arrive about
 * one per frame - a take fills against the seeded depth and the seeded depth
 * itself never aggresses - so crossing a cap of N needs a window spanning at
 * least N+1 frames.
 *
 * @param run The session, for the windows its rules actually got. Read back
 *        rather than recomputed, because they are powers of two and what an
 *        operator asked for is not what a rule uses.
 * @param settings For whether the strategy takes, which decides whether an OTR
 *        cap can mean anything at all.
 * @param cadence The interval between the venue's pushes.
 *
 * @note An estimate about frame-driven fills, not a proof. It is stated as a
 *       warning rather than an error for that reason - and because a cap sized
 *       for a busier market is a legitimate thing to leave configured.
 */
void warn_unreachable_caps(const serving_session &run,
						   const serve_settings &settings,
						   std::chrono::milliseconds cadence) {
	const auto &limits    = run.monitor().limits();
	const auto cadence_ns = to_ns(cadence);
	if (cadence_ns == 0) return;

	if (has_burst_limit(limits)) {
		const std::uint64_t window = run.monitor().fills().window_ns();
		const std::uint64_t needed =
			(static_cast<std::uint64_t>(limits.max_executions_per_window) +
			 1U) *
			cadence_ns;
		if (window < needed)
			spdlog::warn("--max-fills-per-window {} cannot be crossed: its "
						 "window is {} ms and crossing it needs about {} ms at "
						 "this cadence. Widen it with --burst-window-ms",
						 limits.max_executions_per_window,
						 to_ms(window).count(),
						 to_ms(needed).count());
	}

	if (has_ratio_limit(limits)) {
		const std::uint64_t window   = run.monitor().ratio().window_ns();
		const std::uint64_t messages = window / cadence_ns;
		if (messages < limits.min_messages_to_judge)
			spdlog::warn("--max-otr {} cannot be judged: its window is {} ms, "
						 "which holds about {} messages at this cadence, and "
						 "the floor is {}. Widen --otr-window-ms or lower "
						 "--min-otr-messages",
						 limits.max_messages_per_execution,
						 to_ms(window).count(),
						 messages,
						 limits.min_messages_to_judge);
		if (settings.take)
			spdlog::warn(
				"--max-otr {} is measuring a taker, which fills every "
				"order it sends - so its ratio is about 1:1 and no cap "
				"above one can trip. An order-to-trade ratio is a rule "
				"about quoting churn; pass --quote for it to mean "
				"something",
				limits.max_messages_per_execution);
	}
}

/**
 * @brief Log what the run did, from the far end of the loop rather than the
 *        near one - every number here is counted where it landed.
 *
 * @param run The session; its counters are members of an object this frame
 * owns, so they survive an interrupt.
 * @param feed What the pipeline returned, or nothing if it never did.
 *
 * @note The feed's report is @c std::optional and not merely zeroed, because an
 *       interrupt abandons the coroutine that would have delivered it. Printing
 *       its default reads as "no frames arrived", which is the opposite of what
 *       happened - so an absent report says so, and the bridge's own event
 * count below is what a reader should go to instead.
 */
void report_run(const serving_session &run,
				const std::optional<live_feed_report> &feed) {
	const auto &r = run.report();
	if (feed)
		spdlog::info("feed: {} frames ({} malformed), {} reconnects, snapshots "
					 "{}/{} applied - {}",
					 feed->frames,
					 feed->malformed,
					 feed->reconnects,
					 feed->snapshots_applied,
					 feed->snapshots_requested,
					 feed->stopped);
	else
		spdlog::info("feed: report unavailable - the run was interrupted with "
					 "the pipeline still suspended, so it never returned one. "
					 "The bridge's event count below is what arrived");
	spdlog::info("bridge: {} events, {} snapshots ({} gaps, {} invalidations), "
				 "{} depth commands",
				 r.events,
				 r.snapshots,
				 r.gaps,
				 r.invalidations,
				 r.depth_commands);
	// `no_room` is here rather than left on the quoter because a report full of
	// zeroes has exactly one innocent explanation and this is it: the venue
	// quoted tighter than twice --improve, so there was never a price to rest
	// at. Nobody reaches that conclusion from "0 quotes".
	spdlog::info("strategy: {} quotes, {} takes, {} commands accepted, {} "
				 "frames with no room to quote, {} sink stalls",
				 run.quoter().quotes(),
				 run.quoter().takes(),
				 run.quoter().submitted(),
				 run.quoter().no_room(),
				 r.stalls);
	// Printed only when the run was a simulation, and labelled as one. A line
	// that said "0 inferred" on a production run would invite the reading that
	// the model looked and found nothing, which is the opposite of the truth.
	if (run.options().simulate_fills)
		// The frame count comes first on purpose. Every other number here is
		// gated on the venue having crossed one of our prices, so they all read
		// zero on a quiet market *and* on a broken pipeline; the frame count is
		// what tells those two apart, and a reader should see it before the
		// zeroes rather than after them.
		spdlog::info("simulated: {} frames inferred, {} aggressors offering {} "
					 "lots, {} lots queued ahead of us - trade-through {}, "
					 "queue model {}. These fills are a judgement, not the "
					 "engine's",
					 r.inferred_frames,
					 r.injected_aggressors,
					 r.injected_lots,
					 r.queue_absorbed_lots,
					 run.options().fills.require_trade_through ? "on" : "off",
					 run.options().fills.model_queue_position ? "on" : "off");
	spdlog::info("risk: {} passed, {} refused, {} events routed back, position "
				 "{} lots, pnl {} tick-lots",
				 run.gate().passed(),
				 run.gate().refused(),
				 r.engine_events,
				 run.positions().net_lots(run.symbol()),
				 run.gate().pnl());
	// Which rule, and not just how many. A refusal count on its own says the
	// gate is doing something; it does not say whether the number is the collar
	// trimming the far end of the venue's depth (ordinary) or the position
	// limit binding (worth knowing). Only the rules that fired are listed, so a
	// run with nothing configured prints nothing.
	for (const risk::hooks::breach rule : risk::hooks::ALL_BREACHES) {
		if (rule == risk::hooks::breach::NONE) continue;
		const std::uint64_t hits = run.gate().breaches(rule);
		if (hits != 0) spdlog::info("  refused by {}: {}", rule, hits);
	}
	// The windows the caps were actually measured over, read back from the
	// objects rather than from the settings: they are powers of two, so what an
	// operator asked for and what the rule used are not the same number.
	const auto &limits = run.monitor().limits();
	if (has_burst_limit(limits) || has_ratio_limit(limits))
		spdlog::info(
			"  windows: burst {} ms over {} executions, otr {} ms over "
			"{} messages",
			to_ms(run.monitor().fills().window_ns()).count(),
			limits.max_executions_per_window,
			to_ms(run.monitor().ratio().window_ns()).count(),
			limits.min_messages_to_judge);
	spdlog::info("surveillance: {} executions, {} messages, {} trips; feed "
				 "watchdog {} beats, {} trips",
				 run.monitor().fills().total_executions(),
				 run.monitor().ratio().total_messages(),
				 run.monitor().trips(),
				 run.feed_watchdog().beats(),
				 run.feed_watchdog().trips());
	if (run.breaker().state() != risk::hooks::system::trading_state::NORMAL)
		spdlog::warn("the breaker is open: {} ({})",
					 run.breaker().state(),
					 run.breaker().cause());
}

} // namespace

int cmd_serve(const serve_settings &settings,
			  const core::metrics::settings &metrics_settings) {
	namespace affinity  = core::concurrency::affinity;
	namespace binance   = market_data::binance;
	namespace lifecycle = event::lifecycle;
	namespace metrics   = core::metrics;

	if (settings.seconds < 0) {
		spdlog::error("--seconds must not be negative (got {})",
					  settings.seconds);
		return EXIT_FAILURE;
	}

	const auto cadence = binance::from_string(settings.speed);
	if (!cadence) {
		spdlog::error("unknown --speed \"{}\": want {} or {}",
					  settings.speed,
					  binance::depth_speed::every_100ms,
					  binance::depth_speed::every_1000ms);
		return EXIT_FAILURE;
	}

	// --- reference data, before anything is measured against it -------------
	// The venue's grid first, because the flags' defaults are a guess and a wrong
	// step size is silent: surplus precision is truncated on the way in, so a lot
	// coarser than the venue's rounds small levels to zero and still reports a
	// clean parse. @see fetch_venue_grid
	const std::optional<binance::symbol_filters> venue_grid =
		settings.venue_grid ? fetch_venue_grid(settings) : std::nullopt;
	const auto listing = make_listing(settings, venue_grid);
	if (!listing) return EXIT_FAILURE;
	const symbol_spec &spec = *listing;

	// --- the engine's counters ----------------------------------------------
	// Declared unconditionally - it is a few cache lines on the stack - but
	// only wired into the partition when the operator asked for metrics, so a
	// run that never mentions --metrics-enabled pays for an unmetered
	// partition.
	execution::partition_metrics engine_metrics{
		// The settings are plain integers because that is what an INI file and a
		// command line hold; the conversion into durations happens here, once,
		// which is the only place both spellings are in scope.
		.drain_latency_ns{metrics::latency_budgets{
			.p99  = std::chrono::nanoseconds{metrics_settings.drain_p99_budget_ns},
			.p999 = std::chrono::nanoseconds{metrics_settings.drain_p999_budget_ns},
			.max  = std::chrono::nanoseconds{metrics_settings.drain_max_budget_ns},
		}},
	};

	serving_session run(
		spec,
		policy_from(settings,
					metrics_settings.enabled ? &engine_metrics : nullptr));
	// It is what the pipeline drives, and that is a compile-time fact rather
	// than a hope: the four snapshot members on a session exist to satisfy this
	// and nothing else calls them.
	static_assert(live_handler<serving_session>,
				  "a live session must be drivable by run_live_feed");

	// --- cores --------------------------------------------------------------
	// The SPSC hand-off pays real cross-core coherency traffic; sharing one
	// core between the two ends makes every latency figure a measurement of the
	// scheduler. Which cores were reserved has to be recoverable from the log
	// for the same reason it does in `demo`.
	affinity::core_allocator cores(affinity::discover());
	// Named for what the threads do rather than for which end of the queue they
	// hold, and the *reservation* key carries those names rather than only this
	// log line: `pin_this_thread_to` reports the placement under the key it was
	// given, so a mismatch prints four roles for two threads.
	const auto feed_core     = cores.reserve("feed");
	const auto matching_core = cores.reserve("matching");
	const auto core_str      = [](std::optional<affinity::core_id> c) {
		return c ? fmt::to_string(*c) : std::string("any");
	};
	// Distinct *physical* cores, which is `reserve`'s default and the reason
	// these are 0 and 2 on an SMT box rather than 0 and 1: logical 0 and 1 are
	// hyperthread siblings, and an SPSC hand-off between siblings thrashes one
	// core's L1 instead of paying honest cross-core coherency traffic.
	spdlog::info("{}  (feed->cpu {}, matching->cpu {})",
				 cores.get_topology(),
				 core_str(feed_core),
				 core_str(matching_core));

	const auto opened_at = wall_now();
	const auto session   = static_cast<lifecycle::session_id_t>(
        opened_at.time_since_epoch().count());
	spdlog::info("{}",
				 lifecycle::startup{.session   = session,
									.timestamp = opened_at,
									.mode = lifecycle::StartMode::COLD});
	// The grid from the *spec*, not from the flags. They differ whenever the venue
	// was asked, which is the default - and a startup line that echoed the flags
	// while the run used something else would be the most misleading line in the
	// log. @see fetch_venue_grid
	spdlog::info("serving {} at {} (tick {}, lot {} at {}/{} dp), {} liquidity",
				 settings.symbol,
				 *cadence,
				 venue_grid ? venue_grid->tick_size : settings.tick,
				 venue_grid ? venue_grid->step_size : settings.lot,
				 spec.price_scale(),
				 spec.qty_scale(),
				 settings.take ? "taking" : "quoting");

	warn_unreachable_caps(run, settings, binance::interval(*cadence));

	// --- the matching thread ------------------------------------------------
	// Owns exactly one member of the session, which is the whole threading
	// contract. @see live_session.hpp
	std::atomic<bool> stopping{false};
	std::thread matching([&] { match_until_stopped(run, stopping, cores); });

	// --- the feed thread, which is this one ---------------------------------
	if (!cores.pin_this_thread_to("feed"))
		spdlog::warn("the feed thread is unpinned; its latency figures are not "
					 "comparable with a pinned run");

	metrics::registry registry;
	if (metrics_settings.enabled) {
		registry.add("engine_commands_processed",
					 engine_metrics.commands_processed);
		registry.add("engine_trades_emitted", engine_metrics.trades_emitted);
		registry.add("engine_misroutes", engine_metrics.misroutes);
		registry.add("engine_drain_latency_ns",
					 engine_metrics.drain_latency_ns);
	}

	asio::io_context ioc;
	std::optional<live_feed_report> feed_report;
	std::string failure;
	bool interrupted = false;

	// Declared before the coroutine that cancels it. A persistent command needs
	// an operator's way out, and this is the one every process already has.
	asio::signal_set signals(ioc, SIGINT, SIGTERM);

	// One thread runs this context, and it is a requirement rather than a
	// simplification: the frame chain and the snapshot chain both reach the
	// session without a lock, and what makes that sound is that they cannot be
	// running at once. @see app/live_feed.hpp
	asio::co_spawn(ioc,
				   run_live_feed(settings.symbol,
								 &run,
								 feed_options_from(settings, *cadence)),
				   [&](const std::exception_ptr &ep, live_feed_report r) {
					   if (ep) {
						   try {
							   std::rethrow_exception(ep);
						   } catch (const std::exception &e) {
							   failure = e.what();
						   }
					   } else {
						   feed_report = std::move(r);
					   }
					   // The feed is the run. Two things are still parked on
					   // this context - the signal wait and the metrics timer -
					   // and each is outstanding work, so `run()` would never
					   // return without this. Stopping here abandons nothing:
					   // the coroutine that mattered has already finished and
					   // handed back its report.
					   ioc.stop();
				   });

	// On the metrics interval whether or not metrics are enabled: the interval
	// is "how often should this process say something", and a deployment that
	// writes no exposition file still wants a log it can watch.
	asio::co_spawn(
		ioc,
		report_progress(std::chrono::milliseconds(metrics_settings.interval_ms),
						&run),
		asio::detached);

	if (metrics_settings.enabled)
		asio::co_spawn(ioc,
					   publish_metrics(std::chrono::milliseconds(
										   metrics_settings.interval_ms),
									   &registry,
									   metrics_settings.output_file),
					   asio::detached);

	// On an interrupt the context is stopped with the feed coroutine still
	// suspended, so it is abandoned rather than unwound and its own report is
	// lost. That is why the summary below is built from the *session's*
	// counters - they are members of an object this frame owns - and the feed's
	// are reported as far as they got.
	signals.async_wait([&](const boost::system::error_code &ec, int) {
		if (ec) return;
		interrupted = true;
		spdlog::info("interrupted; draining");
		ioc.stop();
	});

	ioc.run();

	// The feed has stopped, so nothing more will be submitted. Tell the
	// matching thread, let it finish, and then route whatever it published on
	// its way out - in that order, because the last events cannot be routed
	// until the thread that publishes them has stopped producing more.
	stopping.store(true, std::memory_order_relaxed);
	matching.join();
	run.pump_all();

	const auto closed_at = wall_now();
	spdlog::info("{}",
				 lifecycle::shutdown{
					 .session   = session,
					 .timestamp = closed_at,
					 // HALTED rather than CLEAN on an interrupt, and the
					 // distinction is what StopReason is for: the queue was
					 // drained either way, but a run whose feed coroutine was
					 // abandoned mid-flight is one a reader should believe less
					 // than one that reached its own duration.
					 .reason = interrupted ? lifecycle::StopReason::HALTED
										   : lifecycle::StopReason::CLEAN,
					 .commands_applied = run.gate().passed(),
					 .events_published = run.report().engine_events,
				 });

	report_run(run, feed_report);

	if (metrics_settings.enabled) {
		fmt::println("drain latency: {}",
					 engine_metrics.drain_latency_ns.read());
		write_exposition(registry, metrics_settings.output_file);
		spdlog::info("wrote metrics to {}", metrics_settings.output_file);
	}

	if (!failure.empty()) {
		spdlog::error("the live feed threw: {}", failure);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

} // namespace exchange::app
