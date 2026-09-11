#include "serve.hpp"

#include "app/cadence_option.hpp"
#include "app/credentials_option.hpp"
#include "core/chrono/wall.hpp"
#include "core/concurrency/affinity.hpp"
#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/logging.hpp"
#include "core/metrics.hpp"
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "core/scaled/fixed_point.hpp"
#include "core/util/owned_file.hpp"
#include "event/lifecycle/lifecycle.hpp"
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "increment.hpp"
#include "market_data/binance/depth_speed.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "session/engine_logger.hpp"
#include "session/gate_logger.hpp"
#include "session/live_feed.hpp"
#include "session/live_session.hpp"
#include "session/reaction_metrics.hpp"
#include "session/user_data_feed.hpp"
#include "session/venue_gateway.hpp"
#include "symbol/symbol_spec.hpp"
#include "symbol/validation.hpp"
#include "transport/rest/pipeline.hpp"
#include "venue/binance/api_error.hpp"
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
#include <fmt/std.h> // IWYU pragma: keep - fmt::formatter<std::filesystem::path>
#include <spdlog/spdlog.h>

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
#include <utility>
#include <vector>


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
	live_session<core::chrono::steady_nanos, gate_logger, engine_logger>;

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
		auto [error] = co_await timer.async_wait(session::detail::TOKEN);
		if (error) co_return;
		write_exposition(*registry, file);
	}
}

/**
 * @brief Ask the venue for @p settings' listing grid.
 *
 * @return What the venue published, or nothing if it could not be asked - in
 *         which case the caller falls back to the flags and says so. A failure
 *         here is deliberately not fatal: the grid is an improvement on a
 * guess, not a precondition for running, and a research tool that refuses to
 *         start because a REST endpoint blinked is worse than one that starts
 * on a stated assumption.
 *
 * Blocking, and that is safe precisely here: this runs before the io_context
 * exists, so there is no loop to stall. It is also the last moment it *can*
 * run, because the listing it produces is what every subsequent number is
 * quantised against. @see make_listing
 */
[[nodiscard]] std::optional<venue::binance::symbol_filters>
fetch_venue_grid(const serve_settings &settings) {
	namespace binance = market_data::binance;

	auto [host, target] =
		venue::binance::exchange_info_endpoint(settings.symbol, settings.env);
	spdlog::debug("GET {}{}", host, target);
	const auto fetched =
		transport::rest::get(std::move(host), std::move(target));
	if (!fetched) {
		spdlog::warn(
			"could not read {}'s grid from the venue: {}",
			settings.symbol,
			venue::binance::describe_api_error(fetched.error().body,
											   fetched.error().message()));
		return std::nullopt;
	}

	auto filters =
		venue::binance::parse_exchange_info(fetched->body, settings.symbol);
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
	spdlog::info(
		"venue grid for {}: tick {} ({} dp), step {} ({} dp), status {}",
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
			 const std::optional<venue::binance::symbol_filters> &grid) {
	// The venue's grid wins where we have it, because it is the grid the
	// numbers on the wire are quantised to and the flags are a guess at it.
	// Precision is the half that matters: surplus decimals are *truncated* on
	// the way in, so a step configured coarser than the venue's silently rounds
	// small levels to nothing. @see venue::binance::symbol_filters
	//
	// Named `grid` and not `venue`: the latter now names a namespace, and a
	// parameter shadowing it would hide every venue:: lookup in this function.
	const std::string tick = grid ? grid->tick_size : settings.tick;
	const std::string lot  = grid ? grid->step_size : settings.lot;
	const int price_decimals =
		grid ? grid->price_decimals : settings.price_decimals;
	const int qty_decimals = grid ? grid->qty_decimals : settings.qty_decimals;

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
	options.duration       = std::chrono::seconds(settings.seconds);
	options.limit          = settings.limit;
	options.price_decimals = settings.price_decimals;
	options.qty_decimals   = settings.qty_decimals;
	options.speed          = cadence;
	// The one value the depth stream, the snapshot fetch and the order path all
	// read. They have to agree: testnet keeps its own book, so a run reading
	// production depth while placing orders there is a strategy reacting to a
	// market it is not trading in. @see venue::environment
	options.env    = settings.env;
	options.verify = settings.insecure_tls ? transport::tls_verify::none
										   : transport::tls_verify::peer;
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
asio::awaitable<void> deliver_on_time(serving_session *run) {
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
 * @brief The venue's minimum order *value*, at the scale a price-times-quantity
 *        product carries.
 *
 * @param grid What the venue published, or nothing when it could not be asked.
 * @param spec The listing, for the two scales whose sum this is read at.
 * @return The floor, or 0 to disable the check - which is what an absent grid
 *         and an absent filter both mean. Guessing one would refuse orders the
 *         venue would have taken, and this refusal is meant to be strictly
 *         narrower than the venue's own.
 *
 * @note The combined scale is not a convenience. @c price_scaled multiplied by
 *       @c qty_scaled carries the sum of their scales, so reading the floor at
 *       that scale is what makes the comparison exact integer arithmetic
 *       instead of a conversion through a double.
 */
[[nodiscard]] std::int64_t
notional_floor(const std::optional<venue::binance::symbol_filters> &grid,
			   const symbol_spec &spec) {
	if (!grid || grid->min_notional.empty()) return 0;
	const int scale = spec.price_scale() + spec.qty_scale();
	const auto floor = core::scaled::parse_fixed_point(grid->min_notional,
													   scale);
	if (!floor) {
		// Unreadable rather than absent, which is a different thing and worth
		// a line: the filter was there and we could not use it, so the check
		// this run makes is weaker than the one it was meant to make.
		spdlog::warn("could not read {}'s minimum notional '{}' - orders will "
					 "not be checked against it before sending",
					 grid->symbol,
					 grid->min_notional);
		return 0;
	}
	spdlog::info("venue minimum order value for {}: {} - anything smaller is "
				 "refused here rather than by the venue",
				 grid->symbol,
				 grid->min_notional);
	return *floor;
}

/// @brief What the order shipper did with what the router queued.
struct shipper_stats {
	std::uint64_t sent     = 0; ///< requests written to the venue
	std::uint64_t accepted = 0; ///< answered 2xx
	std::uint64_t refused  = 0; ///< answered, but not with a 2xx
	std::uint64_t failed   = 0; ///< never answered at all

	/// @brief Cancels the venue answered with "unknown order" - the order had
	///        already filled. Not a failure; @see venue::binance::UNKNOWN_ORDER
	std::uint64_t cancels_too_late = 0;
};

/**
 * @brief Apply one batch's answers to the session and the counters.
 *
 * @param run The session, for the rate-limit headers and for the placements it
 *        has to be told were refused.
 * @param batch What was sent, in order.
 * @param answers What came back. One per request, in the same order - which is
 *        the pipeline's contract and the only thing relating an answer to the
 *        order it is about. @see request_pipeline
 * @param stats Where each outcome is counted.
 *
 * Shared by the running shipper and the withdrawal at exit rather than written
 * twice: the two send the same kind of request to the same venue, and a
 * divergence between them would be a rule about rate limits or refusals that
 * held during a run and not on the way out of one.
 */
void record_answers(serving_session *run,
					std::span<const session::outbound_request> batch,
					std::span<const transport::rest::response> answers,
					shipper_stats *stats) {
	for (std::size_t i = 0; i < answers.size(); ++i) {
		const transport::rest::response &answer = answers[i];
		const session::outbound_request &sent_for =
			batch[std::min(i, batch.size() - 1)];
		const auto now = venue_gateway::clock::now();

		if (answer) {
			++stats->accepted;
			// The venue's own running total, adopted over our estimate.
			// Carried on every response, which is why it is read here rather
			// than only on a refusal. @see venue_gateway::observe
			run->observe_venue(answer->headers, now);
			continue;
		}

		const transport::rest::failure &why = answer.error();
		run->observe_venue(why.headers, now);
		if (why.status == 0) {
			++stats->failed;
			// Written and never answered. Not re-sent, and that is
			// `may_resend`'s rule rather than a shortcut: a placement in this
			// state may or may not be resting at the venue, and the only
			// honest way to find out is to ask what is working.
			// @see session::reconcile
			spdlog::error("order not answered: {} - it may or may not have "
						  "reached the venue",
						  why.message());
			continue;
		}

		// A cancel the venue answers with "unknown order" is not a refusal.
		// The order filled between the quoter deciding to withdraw it and the
		// request landing, which on a fast listing is the ordinary case - and
		// the outcome asked for, the order no longer working, is exactly what
		// happened. Counted apart from the failures and logged at debug,
		// because a run that fills briskly would otherwise bury its real
		// problems under warnings about the market working.
		// @see venue::binance::UNKNOWN_ORDER, and cmd_account, which draws the
		// same line for the same reason.
		const auto refusal = venue::binance::parse_api_error(why.body);
		const bool already_gone = !sent_for.is_placement && refusal &&
								  refusal->code == venue::binance::UNKNOWN_ORDER;
		if (already_gone) {
			++stats->cancels_too_late;
			spdlog::debug("cancel arrived after the order was gone: {}",
						  sent_for.order_id);
			continue;
		}

		++stats->refused;
		spdlog::warn("the venue refused an order: {}",
					 venue::binance::describe_api_error(why.body,
														why.message()));
		// The engine has to be told, because nothing else will: a placement
		// the venue refused never became an order, so the account stream has
		// no lifecycle to report for it. Left unsaid, the gate goes on
		// screening every later size against exposure that does not exist.
		//
		// Placements only, and the exclusion is load-bearing: a refused cancel
		// names an order that may have *filled*, and reporting that as
		// rejected would retire a live position from the gate's ledger.
		// @see live_session::on_send_refused
		if (sent_for.is_placement && sent_for.order_id != 0)
			run->on_send_refused(sent_for.order_id);
		if (why.is_ip_banned()) {
			// Every subsequent request fails for as long as the ban lasts, so
			// quoting into it writes orders nobody will take while the engine
			// records them as working. HALTED rather than CANCEL_ONLY: a
			// cancel would be refused too.
			spdlog::error("this address is banned by the venue; halting order "
						  "entry");
			run->breaker().trip(risk::hooks::system::trading_state::HALTED,
								risk::hooks::system::trip_cause::OPERATOR);
		}
	}
}

/**
 * @brief Take what the router has queued and put it on a socket, until the
 *        context stops.
 *
 * @param run The session, for its outbox and for the rate-limit headers coming
 *        back.
 * @param host The venue's REST host for this run's environment.
 * @param verify Certificate policy - @c peer here whatever the feed was told.
 * @param stats Where the outcome of each request is counted.
 *
 * @par Why a pipeline rather than @c https_request per order
 * Because the connection is the expensive half and an order is small. A
 * one-shot request pays a resolve, a TCP connect and a TLS handshake before it
 * sends a byte - a hundred milliseconds or so to a venue, against a quoter that
 * requotes ten times a second. @c request_pipeline opens once and keeps it, so
 * everything after the first order costs one round trip.
 *
 * @par Why the window is one
 * Ordering. A CANCEL written after a PLACE must reach the venue after it, and
 * pipelining several requests does preserve their order - but @c
 * degrade_on_failure re-sends unanswered *idempotent* requests one at a time on
 * a fresh connection, which reorders a batch around a POST that may not be
 * re-sent. A window of one still keeps the connection, which is the saving that
 * matters, and gives up only the round-trip division on a burst.
 *
 * @par What a response is, and what it is not
 * It is an acknowledgement of receipt, and this treats it as nothing more. What
 * the order *did* comes back on the account stream, which is the venue's own
 * ordered record and arrives whether or not this response ever does. So a 2xx
 * here is counted and discarded; only a refusal is worth a line, because it is
 * the one thing the account stream will never mention - an order the venue
 * declined was never an order.
 *
 * @note Runs on the io_context's thread, which is the session's producer
 *       thread. That is what makes @c take_outbound safe: the frame path that
 *       fills the outbox and this loop that empties it are the same thread, and
 *       cannot be inside it at once.
 */
asio::awaitable<void> ship_orders(serving_session *run, std::string host,
								  transport::tls_verify verify,
								  shipper_stats *stats) {
	/// Long enough that an idle order path is not a spin, short enough to be
	/// lost in the round trip that follows it.
	static constexpr auto kIdleTick = std::chrono::milliseconds{1};

	transport::rest::request_pipeline wire(
		std::move(host),
		// Verified, and not by default: `pipeline_options` defaults to
		// unverified because it was written for market-data fan-out. Every
		// request this loop sends carries an API key in a header.
		transport::rest::pipeline_options{.window = 1, .verify = verify});
	asio::steady_timer timer(co_await asio::this_coro::executor);

	std::vector<transport::rest::request> writing;
	for (;;) {
		if (!run->has_outbound()) {
			timer.expires_after(kIdleTick);
			auto [error] = co_await timer.async_wait(session::detail::TOKEN);
			if (error) break; // the context stopped
			continue;
		}

		std::vector<session::outbound_request> batch = run->take_outbound();
		writing.clear();
		writing.reserve(batch.size());
		for (session::outbound_request &queued : batch)
			writing.push_back(std::move(queued.request));

		const std::vector<transport::rest::response> answers =
			co_await wire.send(writing);
		stats->sent += writing.size();

		record_answers(run, batch, answers, stats);
	}
	co_await wire.close();
}

/**
 * @brief Send whatever the session queued on its way out, once.
 *
 * @param run The session, whose outbox already holds the cancels.
 * @param host The venue's REST host.
 * @param verify Certificate policy - @c peer, like everything credentialed.
 * @param stats Where the answers are counted, the same ones a run's sends use.
 *
 * @par Why this is not the running shipper
 * Because by the time it is called the io_context has stopped and that
 * coroutine is gone. This is a fresh connection, one batch, and no loop: there
 * is nothing after it to retry into, and a withdrawal that waited around for a
 * slow venue would be a process that will not exit.
 *
 * @par What it does not establish
 * That the orders are gone. It cannot: the account stream's coroutine stopped
 * with the context, so what this shows is that the venue *accepted* the
 * withdrawal. An order that filled a moment before its cancel landed answers
 * @c -2011, which @c record_answers already reads as the outcome asked for
 * rather than as a failure. @see venue::binance::UNKNOWN_ORDER
 */
asio::awaitable<void> send_withdrawals(serving_session *run, std::string host,
									   transport::tls_verify verify,
									   shipper_stats *stats) {
	if (!run->has_outbound()) co_return;

	transport::rest::request_pipeline wire(
		std::move(host),
		transport::rest::pipeline_options{.window = 1, .verify = verify});

	const std::vector<session::outbound_request> batch = run->take_outbound();
	std::vector<transport::rest::request> writing;
	writing.reserve(batch.size());
	for (const session::outbound_request &queued : batch)
		writing.push_back(queued.request);

	const std::vector<transport::rest::response> answers =
		co_await wire.send(writing);
	stats->sent += writing.size();
	record_answers(run, batch, answers, stats);
	co_await wire.close();
}

/// @brief Translate the CLI's numbers into the session's policy objects.
[[nodiscard]] live_session_options
policy_from(const serve_settings &settings,
			execution::partition_metrics *metrics,
			session::reaction_metrics *reaction) {
	live_session_options options;

	options.quoting.improve_ticks =
		static_cast<price_t>(settings.improve_ticks);
	options.quoting.lots = static_cast<quantity_t>(settings.lots);
	options.quoting.requote_interval_ns =
		to_ns(std::chrono::milliseconds{settings.requote_ms});
	options.quoting.take_liquidity = settings.take;

	// Both knobs read the flag's *negation*: the model's own defaults are the
	// conservative ones, and a switch a user has to set in order to be
	// flattered is the right way round for a number anybody will act on.
	options.simulate_fills              = settings.simulate_fills;
	options.fills.require_trade_through = !settings.fill_on_lock;
	options.fills.model_queue_position  = !settings.front_of_queue;

	// Left all-zero unless asked for, which builds no wire at all rather than a
	// wire with a zero delay. @see session::latency_pipe
	options.latency.order_entry_ns = settings.latency_ns;
	options.latency.jitter_ns      = settings.jitter_ns;
	if (settings.seed != 0) options.latency.seed = settings.seed;

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
	// Read only by a session that is later given a gateway, which is what keeps
	// a run that did not ask for order entry the run it has always been.
	options.venue_symbol = settings.symbol;

	options.metrics  = metrics;
	options.reaction = reaction;
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
		auto [error] = co_await timer.async_wait(session::detail::TOKEN);
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
				const std::optional<live_feed_report> &feed,
				const shipper_stats &shipping,
				const std::optional<user_data_report> &account) {
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
	// Only when a wire was in the chain, and for the same reason the simulated
	// line is conditional: "0 in flight" on a run with no wire would read as a
	// measurement rather than as an absence.
	if (run.pipe().is_modelled())
		spdlog::info("wire: {} ns flight (+{} jitter), {} commands delivered, "
					 "{} still in flight at exit, {} refused for want of "
					 "flight room",
					 run.options().latency.order_entry_ns,
					 run.options().latency.jitter_ns,
					 r.wire_delivered,
					 r.orders_in_flight,
					 r.wire_refusals);
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
	// Printed only when the run could send, and labelled as one. "0 orders sent"
	// on a run with no gateway would read as a measurement of a strategy that
	// wrote nothing, which is the opposite of the truth. Same rule the
	// simulated and wire lines above already follow.
	if (run.router().is_sending()) {
		const session::router_stats routed = run.router().stats();
		const session::gateway_stats sent  = run.router().gateway()->stats();
		spdlog::info("venue out: {} orders offered, {} queued, {} refused by "
					 "the gateway, {} dropped for want of outbox room, {} still "
					 "waiting at exit",
					 routed.offered,
					 routed.queued,
					 routed.refused,
					 routed.discarded,
					 routed.queued_now);
		spdlog::info("           {} placements, {} cancels, {} weight spent; "
					 "{} written, {} accepted, {} refused, {} unanswered, {} "
					 "cancels too late",
					 sent.placed,
					 sent.cancelled,
					 sent.weight_spent,
					 shipping.sent,
					 shipping.accepted,
					 shipping.refused,
					 shipping.failed,
					 shipping.cancels_too_late);
		// An order written and never answered is the one number here that does
		// not settle by itself: it is neither placed nor refused, and only the
		// venue knows which. Said as a warning so it is not read past.
		if (shipping.failed != 0)
			spdlog::warn("           {} order(s) were written and never "
						 "answered - run `account` to find out whether they "
						 "are working",
						 shipping.failed);

		if (account)
			spdlog::info("venue in: {} frames, {} reports, {} malformed, {} "
						 "reconnects, {} subscribes - {}",
						 account->frames,
						 account->reports,
						 account->malformed,
						 account->reconnects,
						 account->subscribes,
						 account->stopped);
		else
			spdlog::info("venue in: report unavailable - the account stream was "
						 "still running when the run stopped. The counts below "
						 "are what it delivered");
		spdlog::info("          {} reports for this listing: {} booked, {} "
					 "confirmed what the engine already had, {} unusable; {} "
					 "lots filled, {} stream gap(s)",
					 r.venue_reports,
					 r.venue_booked,
					 r.venue_confirmations,
					 r.venue_unusable,
					 r.venue_filled_lots,
					 r.venue_gaps);
		// Separate from the reports above because it did not come from the
		// account stream: a refused placement never became an order, so the
		// stream has nothing to say about it and the HTTP response is the whole
		// notification. @see live_session::on_send_refused
		if (r.venue_refused != 0)
			spdlog::info("          {} placement(s) the venue refused outright, "
						 "withdrawn from the engine's ledger",
						 r.venue_refused);
		// Said whichever way it went, because "nothing was left resting" is the
		// reassurance the line exists to give and a silent report cannot give
		// it. @see live_session::withdraw_all
		spdlog::info("          {} order(s) withdrawn on the way out",
					 r.venue_withdrawn);
	}

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
			spdlog::error("--send-orders and --simulate-fills are alternatives, "
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
			spdlog::warn("--send-orders --live: these are REAL orders on a real "
						 "account, priced by the quoter in this process");
			break;
		case venue::environment::testnet:
			spdlog::info("sending orders to {}: its own book, its own thin "
						 "liquidity",
						 to_string(settings.env));
			break;
		case venue::environment::demo:
			spdlog::info("sending orders to {}: fake balances against depth "
						 "that tracks the live exchange",
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
	// reports a clean parse. @see fetch_venue_grid
	const std::optional<venue::binance::symbol_filters> venue_grid =
		settings.venue_grid ? fetch_venue_grid(settings) : std::nullopt;
	const auto listing = make_listing(settings, venue_grid);
	if (!listing) return EXIT_FAILURE;
	const symbol_spec &spec = *listing;

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

	serving_session run(
		spec,
		policy_from(settings,
					metrics_settings.enabled ? &engine_metrics : nullptr,
					metrics_settings.enabled ? &feed_metrics : nullptr));

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
			.max_orders          = settings.max_orders,
			.weight_reserve      = settings.weight_reserve,
			.min_notional_scaled = notional_floor(venue_grid, spec)},
		&run.breaker());
	if (settings.send_orders) run.attach_gateway(gateway);
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

	const auto opened_at = core::chrono::wall_now();
	const auto session   = lifecycle::session_of(opened_at);
	spdlog::info("{}",
				 lifecycle::startup{.session   = session,
									.timestamp = opened_at,
									.mode      = lifecycle::start_mode::COLD});
	// The grid from the *spec*, not from the flags. They differ whenever the
	// venue was asked, which is the default - and a startup line that echoed
	// the flags while the run used something else would be the most misleading
	// line in the log. @see fetch_venue_grid
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
	shipper_stats shipping;
	std::optional<user_data_report> account_report;
	/// Set when the account stream never opened, which makes the run a failure
	/// rather than a short one. @see the completion handler below.
	bool no_return_leg = false;
	if (settings.send_orders) {
		const std::string rest_host(venue::binance::host_for(settings.env).rest);
		// `peer`, never `settings.insecure_tls`. That flag is about the *feed*,
		// which carries no credential and may have to run on a host with no CA
		// bundle; every request this connection carries has an API key in a
		// header, and a downgrade there is a leak rather than a compatibility
		// setting. @see transport::tls_verify
		asio::co_spawn(ioc,
					   ship_orders(&run,
								   rest_host,
								   transport::tls_verify::peer,
								   &shipping),
					   asio::detached);

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
								  .duration = std::chrono::seconds(0),
								  .verify   = transport::tls_verify::peer}),
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
					spdlog::error("--send-orders needs it: without a return leg "
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
			// A fresh context: the one above has stopped, and with it the
			// shipper that would otherwise have carried these.
			ioc.restart();
			asio::co_spawn(
				ioc,
				send_withdrawals(
					&run,
					std::string(venue::binance::host_for(settings.env).rest),
					transport::tls_verify::peer,
					&shipping),
				asio::detached);
			ioc.run();
		} else if (run.router().is_sending()) {
			spdlog::info("nothing of ours was working at exit");
		}
	}

	// The feed has stopped, so nothing more will be submitted. Tell the
	// matching thread, let it finish, and then route whatever it published on
	// its way out - in that order, because the last events cannot be routed
	// until the thread that publishes them has stopped producing more.
	stopping.store(true, std::memory_order_relaxed);
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
					 // itself short for want of a return leg is the same kind of
					 // thing - it stopped rather than finished, and a record
					 // saying CLEAN would be the one line in the log claiming
					 // otherwise.
					 .reason = is_interrupted || no_return_leg
								   ? lifecycle::stop_reason::HALTED
								   : lifecycle::stop_reason::CLEAN,
					 .commands_applied = run.gate().passed(),
					 .events_published = run.report().engine_events,
				 });

	report_run(run, feed_report, shipping, account_report);

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
