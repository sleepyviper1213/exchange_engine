#pragma once
// What the `serve` command's translation units share, and nothing else.
//
// serve.cpp was one 2100-line file holding the CLI validation, the reference
// data fetch, the order shipper, the venue sweeps and a 280-line report. It is
// now the wiring only - cmd_serve and the coroutines it spawns - and each of
// the four jobs it used to do inline is its own translation unit, declared
// here. A named namespace rather than an anonymous one, because these cross
// translation units, and because a unity batch would otherwise merge them with
// every other command's file-local helpers.

#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "market_data/binance/depth_speed.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "serve.hpp"
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

#include <bit>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace exchange::app::serve_detail {

namespace asio = boost::asio;

using engine::symbol_spec;
using session::engine_logger;
using session::gate_logger;
using session::live_feed_options;
using session::live_feed_report;
using session::live_session;
using session::live_session_options;
using session::live_session_report;
using session::user_data_report;
using session::venue_gateway;
namespace execution = engine::execution;

/// @brief @p ms as a count of nanoseconds, for the risk module's fields.
[[nodiscard]] inline constexpr std::uint64_t
to_ns(std::chrono::milliseconds ms) noexcept {
	return static_cast<std::uint64_t>(std::chrono::nanoseconds{ms}.count());
}

/// @brief @p ns as whole milliseconds, for a log line an operator reads.
///        Truncating, which is the right way round for a width: a 1'048'576 ns
///        window reads as 1 ms rather than as 2.
[[nodiscard]] inline constexpr std::chrono::milliseconds
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
[[nodiscard]] inline unsigned
window_log2_for(std::chrono::milliseconds ms) noexcept {
	const std::uint64_t ns = to_ns(ms);
	if (ns <= 1) return 0;
	return static_cast<unsigned>(std::bit_width(ns - 1));
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

	/// @brief Cancel-replaces, by what the venue said each one left behind.
	///        @see venue::binance::replace_failure
	std::uint64_t replaced          = 0;
	std::uint64_t replace_unchanged = 0;
	std::uint64_t replace_gone      = 0;
	std::uint64_t replace_withdrawn = 0;
};

/**
 * @brief Run @p tick every @p period until the io_context stops.
 *
 * Cancellation arrives as an error code from the timer when the context stops,
 * which is the signal to return - whatever must happen on the way out happens
 * in cmd_serve, where the session is quiescent. @p tick is held by value in
 * the coroutine frame, so a stateful one keeps its state between ticks.
 */
inline asio::awaitable<void>
periodically(std::chrono::milliseconds period,
			 core::util::move_only_function<void()> tick) {
	asio::steady_timer timer(co_await asio::this_coro::executor);
	for (;;) {
		timer.expires_after(period);
		auto [error] = co_await timer.async_wait(session::detail::TOKEN);
		if (error) co_return;
		tick();
	}
}

// --- serve_config.cpp ------------------------------------------------------
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
fetch_venue_grid(const serve_settings &settings);

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
			 const std::optional<venue::binance::symbol_filters> &grid);

/// @brief Translate the CLI's feed options, cadence already validated.
[[nodiscard]] live_feed_options
feed_options_from(const serve_settings &settings,
				  market_data::binance::depth_speed cadence);

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
			   const symbol_spec &spec);

/**
 * @brief @c --max-loss, stated in USDT, as the tick-lots the gate checks.
 *
 * The operator states money because money is what means the same thing on
 * every listing; the gate keeps checking tick-lots because that is a multiply
 * and a compare on the fill path. @c symbol_spec::notional_within is the one
 * conversion between the two, done once here. @see risk::risk_limits
 *
 * @return The floor, @c NO_LOSS_LIMIT when none was asked for, or nothing when
 *         the request cannot be honoured - which is logged, and fatal, since a
 *         stated loss limit that quietly did not apply is the worst outcome.
 */
[[nodiscard]] std::optional<notional_t>
loss_floor(const serve_settings &settings,
		   const std::optional<venue::binance::symbol_filters> &grid,
		   const symbol_spec &spec);

/// @brief Translate the CLI's numbers into the session's policy objects.
[[nodiscard]] live_session_options
policy_from(const serve_settings &settings, notional_t max_loss,
			execution::partition_metrics *metrics,
			session::reaction_metrics *reaction);

// --- serve_shipper.cpp -----------------------------------------------------
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
					shipper_stats *stats);

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
								  shipper_stats *stats);

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
									   shipper_stats *stats);

// --- serve_venue.cpp -------------------------------------------------------
/**
 * @brief Read what the venue still has working for us, and withdraw it.
 *
 * @par Why the withdrawal above is not enough
 * Because it cancels what the *quoter* believes is live, and that belief is
 * exactly the thing that has been wrong: a testnet run exited "CLEAN" with two
 * quotes still resting, each forgotten after an amendment the venue never
 * received. Asking the venue is the only check that does not trust the
 * process's own bookkeeping. One weight for the read, one per order found.
 *
 * Found orders are reported as a warning rather than quietly cancelled: each
 * one is the engine having lost track of something it placed, which is a bug
 * worth seeing even when the sweep cleaned up after it.
 *
 * @note Only orders carrying our client-id prefix are touched. A shared
 *       account's hand-placed orders are reported and left alone, the same
 *       rule @c account --cancel-all keeps. @see session::CLIENT_ORDER_PREFIX
 */
void sweep_resting_orders(const serve_settings &settings,
						  std::string_view phase);

/**
 * @brief Say, before the first quote, which side the account cannot pay for.
 *
 * @return @c false only when it can pay for neither - a run that could never
 *         place anything. One side short is a warning, not a refusal: balances
 *         move with every fill, so a sell the account cannot cover now may be
 *         covered by the first buy, and the quoter's backoff is what keeps the
 *         refusals in between from eating the order allowance.
 *
 * @par Why it exists
 * A Demo Mode run on an account holding no SOL had every sell refused with
 * @c -2010, about once a second, until the refusals spent @c --max-orders and
 * the last two minutes quoted nothing at all. The log said "insufficient
 * balance" 229 times and never said why the run went quiet.
 *
 * Advisory in every other way: an account or price read that fails is logged
 * and the run goes ahead, since the venue's refusals still say the same thing.
 * Exact integer comparisons at the listing's own scales, never a double.
 */
[[nodiscard]] bool can_afford_either_side(
	const serve_settings &settings,
	const std::optional<venue::binance::symbol_filters> &grid,
	const symbol_spec &spec);

/**
 * @brief Find the fills of ours the account stream never delivered, and say
 *        what the position is with them.
 *
 * @return The net of those fills, in lots - positive is bought.
 *
 * @par Why there are any
 * The exit withdrawal is sent after the io_context has stopped, and with it
 * the account stream. A quote that fills in the moment between the decision
 * to exit and the venue applying the cancel - the cancel then answers "unknown
 * order" - is a fill nothing here will ever hear about. A two-hour Demo Mode
 * run ended with its final position 50 lots out for exactly that reason:
 * `ex-3292` filled 0.6 s into the withdrawal.
 *
 * The venue numbers a listing's trades in order, so anything above the
 * highest trade id the session booked is one it missed; an order lookup per
 * trade keeps only ours. @see live_session::newest_trade_id
 */
[[nodiscard]] std::int64_t
account_for_unreported_fills(const serve_settings &settings,
							 const serving_session &run);

// --- serve_report.cpp ------------------------------------------------------
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
									  const serving_session *run);

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
						   std::chrono::milliseconds cadence);

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
				const std::optional<user_data_report> &account);

} // namespace exchange::app::serve_detail
