#include "app/cadence_option.hpp"
#include "app/commands/venue_orders.hpp"
#include "app/credentials_option.hpp"
#include "app/heartbeat.hpp"
#include "core/chrono/wall.hpp"
#include "core/concurrency/affinity.hpp"
#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/concurrency/synchronisation/stop_handshake.hpp"
#include "core/logging.hpp"
#include "core/metrics.hpp"
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "core/util/owned_file.hpp"
#include "event/lifecycle/lifecycle.hpp"
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "market_data/binance/depth_speed.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "serve_internal.hpp"
#include "session/engine_logger.hpp"
#include "session/gate_logger.hpp"
#include "session/live_feed.hpp"
#include "session/live_session.hpp"
#include "session/reaction_metrics.hpp"
#include "session/user_data_feed.hpp"
#include "session/venue_gateway.hpp"
#include "symbol/symbol_spec.hpp"
#include "transport/rest/pipeline.hpp"
#include "venue/binance/exchange_info.hpp"
#include "venue/binance/host.hpp"
#include "venue/environment.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/ranges.h>
#include <fmt/std.h> // IWYU pragma: keep - fmt::formatter<std::filesystem::path>
#include <spdlog/spdlog.h>

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
#include <utility>

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
using session::run_user_data_feed;
using session::user_data_options;
using session::user_data_report;
using session::venue_gateway;

namespace {

namespace asio = boost::asio;

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
	co_await serve_detail::periodically(every,
										[registry, file = std::move(file)] {
											write_exposition(*registry, file);
										});
}

/**
 * @brief The matching thread's whole life.
 *
 * Owns exactly one member of the session - @c drain_and_publish - which is the
 * whole threading contract. @see live_session.hpp
 *
 * The run ends with a drain to empty: what the producer submitted last may not
 * have been matched yet, and what was matched may not have been published, so
 * draining is what makes the report describe the session rather than where
 * this thread happened to be. @c stop_handshake owns that drain and the two
 * orderings it depends on.
 *
 * @param run The session. Only its consumer-side member is touched here.
 * @param handshake Stopped by the feed thread once nothing more will be
 *        submitted.
 * @param cores The reservation table, for pinning.
 */
void match_until_stopped(
	serve_detail::serving_session &run,
	core::concurrency::synchronisation::stop_handshake &handshake,
	core::concurrency::affinity::core_allocator &cores) {
	if (!cores.pin_this_thread_to("matching"))
		spdlog::warn("the matching thread is unpinned; its latency figures are "
					 "not comparable with a pinned run");

	handshake.run([&]() noexcept { return run.drain_and_publish(); });
}

/**
 * @brief Deliver commands whose modelled flight time has elapsed, until
 *        cancelled.
 *
 * @par Why this coroutine exists at all
 * Because a modelled delay is only a model if something enforces it, and
 * offline the enforcement is free: the harness advances market time itself, so
 * the moment a command comes due is a moment the harness is already awake.
 * Wall-clock time is not like that. It passes during a quiet market, and a
 * command that comes due between two frames comes due with nobody looking. This
 * is the "nobody" - a timer that wakes when the next command is due and hands
 * it over. @see live_session::deliver_due, and TODO 16b, which is this gap.
 *
 * @par How it waits, and the resolution that costs
 * With something in flight it sleeps exactly until that command is due; with
 * nothing in flight there is nothing to compute a deadline from, so it ticks at
 * @c kIdleTick and looks again. The frame path also delivers, so on a busy
 * market this loop is a backstop rather than the mechanism.
 *
 * What neither arrangement can do is beat the platform's timer resolution,
 * which on Windows is a millisecond or so. A modelled delay below that is
 * therefore *at least* honest about direction and not about magnitude - the
 * command is late rather than instant, which is the right sign, but the number
 * is not the one that was asked for. A backtest has no such floor because it
 * owns the clock. This is worth stating rather than discovering: it is the one
 * axis on which the live path and the offline path do not agree, even
 * configured identically.
 *
 * @note Runs on the io_context's thread, which is the session's producer
 *       thread - the only thread allowed to touch anything but
 *       @c drain_and_publish. @see live_session.hpp
 */
asio::awaitable<void> deliver_on_time(serve_detail::serving_session *run) {
	/// Long enough that a quiet market is not a spin, short enough that the
	/// delay it can add is under the resolution a steady timer offers anyway.
	static constexpr auto kIdleTick = std::chrono::milliseconds{1};

	asio::steady_timer timer(co_await asio::this_coro::executor);
	for (;;) {
		const std::optional<std::uint64_t> due = run->next_due_ns();
		if (!due) {
			timer.expires_after(kIdleTick);
		} else {
			// Signed on purpose: a due time already in the past is the normal
			// case after a frame delivered late, and expires_after with a
			// negative duration fires immediately, which is what it should do.
			const auto now = static_cast<std::int64_t>(run->clock().now_ns());
			timer.expires_after(std::chrono::nanoseconds{
				static_cast<std::int64_t>(*due) - now});
		}
		auto [error] = co_await timer.async_wait(session::detail::TOKEN);
		if (error) co_return; // the context stopped; the drain happens on exit
		(void)run->deliver_due();
	}
}

/**
 * @brief Queue a reconciliation read every @p every, until the context stops.
 *
 * Only queues it: the read travels the same one-at-a-time connection as the
 * orders, which is what makes its answer a consistent cut of everything sent
 * before it. @see session::order_router::on_open_orders
 */
asio::awaitable<void>
reconcile_periodically(std::chrono::milliseconds every,
					   serve_detail::serving_session *run) {
	co_await serve_detail::periodically(every, [run] {
		(void)run->request_reconciliation();
	});
}

/**
 * @brief Touch @p path every @p every, until the context stops.
 *
 * On the io_context, deliberately: the thread that runs the feed and the
 * order path is the one whose silence matters. A heartbeat from a thread of
 * its own would go on beating over a run that had wedged.
 * @see app/heartbeat.hpp
 */
asio::awaitable<void> beat(std::filesystem::path path,
						   std::chrono::milliseconds every) {
	const auto touch = [path = std::move(path)] {
		if (!write_heartbeat(path, heartbeat_state::running))
			spdlog::warn("could not write the heartbeat to {}", path.string());
	};
	touch();
	co_await serve_detail::periodically(every, touch);
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

	const auto cadence = cadence_from(settings.speed);
	if (!cadence) return EXIT_FAILURE;

	// --- what this run could do, before it does any of it -------------------
	// Said once at startup because the alternative is finding out later: a run
	// with no credential looks identical to one with a working credential
	// until something tries to place an order. Prints a fingerprint, never a
	// key and never the secret. @see credentials_option.hpp
	spdlog::info("{}", describe(settings.credential));

	// --- order entry, refused before anything else if it cannot be safe -----
	if (settings.send_orders) {
		if (!settings.env_chosen) {
			// The one guard that makes the production *default* safe. This
			// command reads the real book because that is the only book worth
			// measuring against, so `env` defaults to production - and an
			// operator who types `--send-orders` and nothing else would
			// otherwise get production order entry from a default they never
			// saw. Naming all three rather than picking a safe one, because the
			// choice is theirs to make explicitly.
			spdlog::error("--send-orders needs an environment: pass --testnet, "
						  "--demo or --live. The feed defaults to production "
						  "and order entry must never inherit that silently");
			return EXIT_FAILURE;
		}
		if (!settings.credential.is_complete()) {
			spdlog::error("--send-orders needs a credential; set {} and {}",
						  venue::API_KEY_VAR,
						  venue::API_SECRET_VAR);
			return EXIT_FAILURE;
		}
		if (settings.simulate_fills) {
			// Two answers to one question. The model infers the fills a resting
			// order *would* have taken; the account stream reports the ones it
			// did. A run with both books every execution twice, so the position
			// it reports is of a market that does not exist.
			spdlog::error(
				"--send-orders and --simulate-fills are alternatives, "
				"not a combination: one infers fills and the other "
				"receives them, and both together count each twice");
			return EXIT_FAILURE;
		}
		if (settings.take) {
			// The same objection, arriving by the other door. `--take` crosses
			// the touch with an IOC, and the touch it crosses is *in the
			// engine's own book* - depth this process mirrored from the venue
			// and rested there. So the order fills internally against a copy of
			// the venue's liquidity, and then fills again for real and is
			// reported on the account stream. Two fills, one order.
			//
			// `--quote` has no such problem, and the asymmetry is not luck:
			// seeded depth is rested with `add_order`, which does not match, so
			// an order resting inside the touch can never fill against it. The
			// venue's report is then the only execution there is, which is
			// exactly the arrangement order entry needs.
			spdlog::error("--send-orders needs --quote: --take crosses the "
						  "venue's depth mirrored into this engine's own book, "
						  "so every order would fill once here and once at the "
						  "venue");
			return EXIT_FAILURE;
		}
		switch (settings.env) {
		case venue::environment::production:
			spdlog::warn(
				"--send-orders --live: these are REAL orders on a real "
				"account, priced by the quoter in this process");
			break;
		case venue::environment::testnet:
			spdlog::info("sending orders to {}: its own book, its own thin "
						 "liquidity",
						 to_string(settings.env));
			break;
		case venue::environment::demo:
			spdlog::info("sending orders to {}: fake balances against depth "
						 "that tracks the live order_book",
						 to_string(settings.env));
			break;
		}
	}

	// Same argument as the credential line above, for the same reason: a run
	// whose feed nobody authenticated looks exactly like one whose feed was
	// verified, and the difference decides whether the book below is the
	// venue's or somebody else's. Warn rather than info - this is a downgrade
	// the operator asked for, and it should read like one in the log they scan
	// afterwards.
	if (settings.insecure_tls)
		spdlog::warn("--insecure-tls: the feed's certificate is NOT verified, "
					 "so this session trades on a book anyone who can "
					 "terminate the connection may dictate");

	// --- reference data, before anything is measured against it -------------
	// The venue's grid first, because the flags' defaults are a guess and a
	// wrong step size is silent: surplus precision is truncated on the way in,
	// so a lot coarser than the venue's rounds small levels to zero and still
	// reports a clean parse. @see serve_detail::fetch_venue_grid
	const std::optional<venue::binance::symbol_filters> venue_grid =
		settings.venue_grid ? serve_detail::fetch_venue_grid(settings)
							: std::nullopt;
	const auto listing = serve_detail::make_listing(settings, venue_grid);
	if (!listing) return EXIT_FAILURE;
	const symbol_spec &spec = *listing;
	const auto max_loss = serve_detail::loss_floor(settings, venue_grid, spec);
	if (!max_loss) return EXIT_FAILURE;

	// --- the engine's counters ----------------------------------------------
	// Declared unconditionally - it is a few cache lines on the stack - but
	// only wired into the partition when the operator asked for metrics, so a
	// run that never mentions --metrics-enabled pays for an unmetered
	// partition.
	execution::partition_metrics engine_metrics =
		execution::metrics_with_budgets(metrics_settings);

	// --- reaction time ------------------------------------------------------
	// The producer thread's own distribution, kept off the partition's cache
	// lines because the two are written by different threads. No budgets: this
	// path has no committed baseline yet, and a histogram with no budget is
	// always healthy rather than falsely alarming. @see
	// session/reaction_metrics.hpp
	session::reaction_metrics feed_metrics;

	// On the heap, and not because anything here wants to share ownership.
	// A session is ~730 KB of *inline* ring: engine_partition holds a
	// 4096-slot command queue and event_channel an 8192-slot event queue, both
	// with their storage as a member array rather than behind a pointer,
	// because that is what keeps a dequeue off a second cache line. That is
	// most of a default 1 MB thread stack in one frame, and it grows with
	// either capacity. The reference below keeps every use site reading as a
	// value, which is what it is - the pointer exists only to choose the
	// storage. @see spsc_queue, and live_desk in the test tree, which learned
	// this the hard way.
	live_session_options session_options = serve_detail::policy_from(
		settings,
		*max_loss,
		metrics_settings.enabled ? &engine_metrics : nullptr,
		metrics_settings.enabled ? &feed_metrics : nullptr);
	if (venue_grid) {
		session_options.base_asset  = venue_grid->base_asset;
		session_options.quote_asset = venue_grid->quote_asset;
	}
	const auto session_storage =
		std::make_unique<serve_detail::serving_session>(
			spec,
			std::move(session_options));
	serve_detail::serving_session &run = *session_storage;

	// --- the venue's side of the order path ---------------------------------
	// After the session and not before it, because a gateway asks the session's
	// own circuit breaker whether an order may go - so the breaker has to exist
	// first. Declared unconditionally, like the metrics above, and attached
	// only when asked for: an unattached gateway is never consulted and the
	// router never builds a request. @see live_session::attach_gateway
	venue_gateway gateway(
		settings.credential,
		settings.env,
		session::gateway_limits{
			.max_orders     = settings.max_orders,
			.weight_reserve = settings.weight_reserve,
			.min_notional_scaled =
				serve_detail::notional_floor(venue_grid, spec)},
		&run.breaker());
	if (settings.send_orders) {
		run.attach_gateway(gateway);
		// Before the first quote, because engine order ids restart at one: an
		// order a previous run left working as `ex-1` would be
		// indistinguishable from this run's first quote, and its fills would be
		// booked to it.
		serve_detail::sweep_resting_orders(settings, "startup");
		if (!serve_detail::can_afford_either_side(settings, venue_grid, spec))
			return EXIT_FAILURE;
	}
	// It is what the pipeline drives, and that is a compile-time fact rather
	// than a hope: the four snapshot members on a session exist to satisfy this
	// and nothing else calls them.
	static_assert(live_handler<serve_detail::serving_session>,
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

	const auto opened_at = core::chrono::wall_now();
	const auto session   = lifecycle::session_of(opened_at);
	spdlog::info("{}",
				 lifecycle::startup{.session   = session,
									.timestamp = opened_at,
									.mode      = lifecycle::start_mode::COLD});
	// The grid from the *spec*, not from the flags. They differ whenever the
	// venue was asked, which is the default - and a startup line that echoed
	// the flags while the run used something else would be the most misleading
	// line in the log. @see serve_detail::fetch_venue_grid
	spdlog::info("serving {} at {} (tick {}, lot {} at {}/{} dp), {} liquidity",
				 settings.symbol,
				 *cadence,
				 venue_grid ? venue_grid->tick_size : settings.tick,
				 venue_grid ? venue_grid->step_size : settings.lot,
				 spec.price_scale(),
				 spec.qty_scale(),
				 settings.take ? "taking" : "quoting");

	serve_detail::warn_unreachable_caps(run,
										settings,
										binance::interval(*cadence));

	// --- the matching thread ------------------------------------------------
	// Owns exactly one member of the session, which is the whole threading
	// contract. @see live_session.hpp
	core::concurrency::synchronisation::stop_handshake handshake;
	std::thread matching([&] { match_until_stopped(run, handshake, cores); });

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
		registry.add("feed_frame_reaction_ns", feed_metrics.frame_reaction_ns);
		registry.add("feed_resync_reaction_ns",
					 feed_metrics.resync_reaction_ns);
		registry.add("feed_unstamped_messages", feed_metrics.unstamped);
	}

	asio::io_context ioc;
	std::optional<live_feed_report> feed_report;
	std::string failure;
	bool is_interrupted = false;

	// Declared before the coroutine that cancels it. A persistent command needs
	// an operator's way out, and this is the one every process already has.
	asio::signal_set signals(ioc, SIGINT, SIGTERM);

	// One thread runs this context, and it is a requirement rather than a
	// simplification: the frame chain and the snapshot chain both reach the
	// session without a lock, and what makes that sound is that they cannot be
	// running at once. @see session/live_feed.hpp
	asio::co_spawn(
		ioc,
		run_live_feed(settings.symbol,
					  &run,
					  serve_detail::feed_options_from(settings, *cadence)),
		[&](const std::exception_ptr &ep, live_feed_report r) {
			if (ep) {
				try {
					std::rethrow_exception(ep);
				} catch (const std::exception &e) { failure = e.what(); }
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

	// Only when a wire exists. With no modelled latency there is nothing in
	// flight to become due, so the loop would be a millisecond tick that never
	// delivered anything - and a deployment that asked for none of this should
	// pay for none of it. @see session::latency_pipe
	if (run.pipe().is_modelled())
		asio::co_spawn(ioc, deliver_on_time(&run), asio::detached);

	// --- the order path's two coroutines, and only when it is switched on ---
	//
	// Both live on this same io_context, which is what makes the whole
	// arrangement single-threaded: the frame path fills the outbox, the shipper
	// empties it, and the account stream reaches straight into the feedback
	// router. Three coroutines, one thread, no lock between them.
	serve_detail::shipper_stats shipping;
	std::optional<user_data_report> account_report;
	/// Set when the account stream never opened, which makes the run a failure
	/// rather than a short one. @see the completion handler below.
	bool no_return_leg = false;
	if (settings.send_orders) {
		const std::string rest_host(
			venue::binance::host_for(settings.env).rest);
		// `peer`, never `settings.insecure_tls`. That flag is about the *feed*,
		// which carries no credential and may have to run on a host with no CA
		// bundle; every request this connection carries has an API key in a
		// header, and a downgrade there is a leak rather than a compatibility
		// setting. @see transport::tls_verify
		asio::co_spawn(ioc,
					   serve_detail::ship_orders(&run,
												 rest_host,
												 transport::tls_verify::peer,
												 &shipping),
					   asio::detached);
		if (settings.reconcile_ms > 0)
			asio::co_spawn(ioc,
						   reconcile_periodically(
							   std::chrono::milliseconds{settings.reconcile_ms},
							   &run),
						   asio::detached);
		if (!settings.heartbeat_file.empty())
			asio::co_spawn(
				ioc,
				beat(settings.heartbeat_file, std::chrono::milliseconds{1000}),
				asio::detached);
		else
			spdlog::warn("no --heartbeat-file: nothing will withdraw this "
						 "run's orders if it dies - the venue has no "
						 "cancel-on-disconnect for spot");

		// The return leg. Its scales come from the same spec the order path
		// encodes with, so a report is decoded on the grid its order was
		// written on.
		asio::co_spawn(
			ioc,
			run_user_data_feed(
				settings.credential,
				&run,
				user_data_options{.env            = settings.env,
								  .price_decimals = spec.price_scale(),
								  .qty_decimals   = spec.qty_scale(),
								  .duration       = std::chrono::seconds(0),
								  .verify = transport::tls_verify::peer}),
			[&](const std::exception_ptr &ep, user_data_report r) {
				if (ep) return;
				account_report = std::move(r);

				// Two different failures, and they want opposite answers.
				//
				// A stream that never opened at all is a *startup* failure:
				// nothing has been placed, there is nothing to unwind, and a
				// run that carried on would spend its whole duration refusing
				// every order it wrote - and refusing the mirrored depth
				// alongside them, because the gate screens that too. So it
				// stops, and says so, rather than looking like a run.
				if (account_report->frames == 0 &&
					account_report->reconnects == 0) {
					spdlog::error("the account stream never opened: {}",
								  account_report->stopped);
					spdlog::error(
						"--send-orders needs it: without a return leg "
						"this process would place orders and never "
						"hear what became of them");
					no_return_leg = true;
					ioc.stop();
					return;
				}

				// A stream that opened and then gave up is different: orders of
				// ours may be working, and the run is the only thing that can
				// withdraw them. So it continues, blind, with new orders
				// stopped and cancels still passing - which is what
				// `on_gap` does for the same reason. @see live_session::on_gap
				spdlog::error("the account stream has stopped: {}",
							  account_report->stopped);
				run.breaker().trip(
					risk::hooks::system::trading_state::CANCEL_ONLY,
					risk::hooks::system::trip_cause::STALE_WORKING);
			});
	}

	// On the metrics interval whether or not metrics are enabled: the interval
	// is "how often should this process say something", and a deployment that
	// writes no exposition file still wants a log it can watch.
	asio::co_spawn(ioc,
				   serve_detail::report_progress(
					   std::chrono::milliseconds(metrics_settings.interval_ms),
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
		is_interrupted = true;
		spdlog::info("interrupted; draining");
		ioc.stop();
	});

	ioc.run();

	// --- leave nothing of ours resting in the venue's book ------------------
	//
	// Before the matching thread is stopped, and that ordering is the whole of
	// it: a cancel goes through the gate and into the partition's queue like
	// every other command, so the consumer has to still be there to drain it.
	// Stop the thread first and `withdraw_all` spins against a queue nobody is
	// emptying.
	//
	// A GTC quote does not expire with the process that priced it. Left behind,
	// the next thing to happen to it is a fill nobody is watching for, against
	// a position nobody is managing - which is the situation the whole risk
	// lane exists to prevent, arrived at by simply exiting.
	if (settings.send_orders) {
		if (const std::size_t withdrawn = run.withdraw_all(); withdrawn != 0) {
			spdlog::info("withdrawing {} order(s) before exit", withdrawn);
			// A genuinely separate context, and it has to be separate rather
			// than `ioc` restarted.
			//
			// `ioc.stop()` does not cancel the work queued on it - the feed
			// coroutine, the quote timer and the duration timer are all still
			// pending - and `restart()` clears the stopped flag for every one
			// of them, not only for the withdrawal spawned after it. Running
			// it again therefore resumes the whole session: it quotes, it
			// places, and it exits when its duration expires rather than when
			// the operator asked it to. An interrupt that cancels the orders
			// and then carries on trading is the one shutdown this path must
			// not have, and it is what this was until a testnet run kept
			// placing orders for ninety-five seconds after Ctrl+C.
			//
			// `serve_detail::send_withdrawals` builds its own
			// `request_pipeline` and holds nothing bound to the old executor,
			// so a fresh context costs one object and makes the leak impossible
			// by construction.
			asio::io_context withdrawals;
			asio::co_spawn(
				withdrawals,
				serve_detail::send_withdrawals(
					&run,
					std::string(venue::binance::host_for(settings.env).rest),
					transport::tls_verify::peer,
					&shipping),
				asio::detached);
			withdrawals.run();
		} else if (run.router().is_sending()) {
			spdlog::info("nothing of ours was working at exit");
		}
		serve_detail::sweep_resting_orders(settings, "exit");
		(void)serve_detail::account_for_unreported_fills(settings, run);
		// Last, after the sweep: a run that dies anywhere in its own shutdown
		// is still "running" and goes stale, which is when the watchdog acts.
		if (!settings.heartbeat_file.empty() &&
			!write_heartbeat(settings.heartbeat_file, heartbeat_state::stopped))
			spdlog::warn("could not mark the heartbeat stopped - the watchdog "
						 "will treat this exit as a death and sweep again");
	}

	// The feed has stopped, so nothing more will be submitted. Tell the
	// matching thread, keep routing while it finishes - its last drain can
	// publish more than the ring holds, and only this thread empties the ring -
	// and then route whatever it published on its way out.
	handshake.stop([&]() noexcept { return run.pump_all(); });
	matching.join();
	run.pump_all();

	const auto closed_at = core::chrono::wall_now();
	spdlog::info("{}",
				 lifecycle::shutdown{
					 .session   = session,
					 .timestamp = closed_at,
					 // HALTED rather than CLEAN on an interrupt, and the
					 // distinction is what stop_reason is for: the queue was
					 // drained either way, but a run whose feed coroutine was
					 // abandoned mid-flight is one a reader should believe less
					 // than one that reached its own duration. A run that cut
					 // itself short for want of a return leg is the same kind
					 // of thing - it stopped rather than finished, and a record
					 // saying CLEAN would be the one line in the log claiming
					 // otherwise.
					 .reason           = is_interrupted || no_return_leg
											 ? lifecycle::stop_reason::HALTED
											 : lifecycle::stop_reason::CLEAN,
					 .commands_applied = run.gate().passed(),
					 .events_published = run.report().engine_events,
				 });

	serve_detail::report_run(run, feed_report, shipping, account_report);

	if (metrics_settings.enabled) {
		// The number this whole path exists to print: how long the process took
		// to answer a frame, measured from when the frame landed on the box.
		// @see docs/performance.md
		fmt::println(
			"drain latency: {}\nframe reaction: {}\nresync reaction: {}",
			engine_metrics.drain_latency_ns.read(),
			feed_metrics.frame_reaction_ns.read(),
			feed_metrics.resync_reaction_ns.read());
		write_exposition(registry, metrics_settings.output_file);
		spdlog::info("wrote metrics to {}", metrics_settings.output_file);
	}

	if (!failure.empty()) {
		spdlog::error("the live feed threw: {}", failure);
		return EXIT_FAILURE;
	}
	// Reported after the summary rather than instead of it: the run did read a
	// book and the counters above describe what it saw, so they are worth
	// printing. What it did not do is the thing it was asked to do.
	if (no_return_leg) return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

} // namespace exchange::app
