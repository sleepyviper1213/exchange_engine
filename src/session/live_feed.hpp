#pragma once
// The live ingest pipeline: two asynchronous sources, one owner, one thread.
//
// market_data::drive() is the offline half of this - pull a feed until it ends,
// hand each message to a handler. A live feed cannot be written that way, and
// the reason is the whole point of this file: the snapshot is not in the
// stream. A diff feed goes stale on its own schedule, and repairing it means
// issuing a REST request *while frames keep arriving*, because the frames that
// arrive during the fetch are exactly the ones that will bridge the snapshot to
// the present. Stop reading to fetch and the gap you are repairing gets wider
// than the snapshot can close.
//
// So this is a pipeline in the concurrent sense: two chains of asynchronous
// work, overlapping in time. What they are emphatically *not* is two writers.
//
//   ws frames ──▶ decode ──▶ on_event ──▶ depth_reconstructor ──▶ l2_book
//                                             ▲
//   REST fetch ──▶ parse ──▶ channel ─────────┘   (drained by the frame loop)
//
// --- why there is no lock, and why that is not a timing argument -----------
//
// The first version of this had both chains calling into the reconstructor and
// justified it by scheduling: one io_context, one thread, a coroutine yields
// only at a co_await, so the two can never interleave mid-mutation. That is
// true, and it is the wrong kind of true. It makes the design correct because
// of how it happens to be run rather than because of what it is, and it would
// fail silently the day somebody ran the context on two threads.
//
// The replica now has exactly one writer. The fetch chain never touches it: it
// posts its result - success or failure - into a channel, and the frame loop
// drains that channel at a point of its own choosing, once per frame.
// Ownership rather than synchronisation, which is the same rule the matching
// engine's books are built on and the one this codebase states first.
//
// What falls out of it:
//   * the fetch holds a shared_ptr to the channel and nothing else, so it
//     cannot dangle however the pipeline exits - no join is needed for safety,
//     and the poll loop that used to be that join is gone;
//   * "is a fetch outstanding" is now frame-loop-local state rather than shared
//     state, so there is nothing left to race over;
//   * draining once per frame costs nothing: frames arrive every 100 ms and a
//     REST snapshot takes considerably longer than one to fetch.
//
// --- where this lives ------------------------------------------------------
//
// market_data links neither Boost nor transport, and transport must not know
// what a depthUpdate is, so nothing below can host this. It sits with its only
// caller, the way strategy/backtest/depth_feed_bridge.hpp does - and if a
// second caller ever appears, it should move down beside them rather than stay
// in the composition root.

#include "core/logging.hpp"
#include "market_data/binance.hpp"
#include "market_data/feed.hpp"
#include "market_data/normalised.hpp"
#include "transport/rest.hpp"
#include "transport/websocket.hpp"
#include "venue/binance/api_error.hpp"
#include "venue/environment.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <fmt/chrono.h> // IWYU pragma: keep - formats reconnect_delay
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>


namespace exchange::session {

/**
 * @brief A @c feed_handler that also drives the snapshot half of the managed
 *        local-order-book procedure.
 *
 * @c market_data::feed_handler covers the two message kinds and no more, which
 * is all @c drive needs because an offline capture carries its own seed. A live
 * pipeline has to *ask* for one, so it needs a handler that can say whether it
 * wants a snapshot, be told a fetch is in flight, be told it failed, and be
 * told its replica is stale after a reconnect.
 *
 * @c market_data::depth_reconstructor satisfies this as written, which is again
 * the point: the four extra members are its existing public surface, not an
 * interface invented for this file.
 */
template <class H>
concept live_handler = market_data::feed_handler<H> && requires(H &handler) {
	{ handler.needs_snapshot() } -> std::convertible_to<bool>;
	handler.snapshot_requested();
	handler.snapshot_failed();
	handler.invalidate();
};

/// @brief How a live pipeline is configured.
struct live_feed_options {
	/**
	 * @brief How long to run; zero means until the stream ends or fails beyond
	 *        recovery.
	 *
	 * @note Checked between frames, so a feed that has gone quiet overshoots
	 * it. Bounded rather than unbounded: the WebSocket layer runs with Beast's
	 *       suggested client timeouts, so a silent stream fails its idle
	 *       deadline and the loop comes back round. A hard deadline would need
	 *       the read itself to be cancelled.
	 */
	std::chrono::seconds duration{0};
	/**
	 * @brief Levels per side to ask the REST snapshot for.
	 *
	 * Bounded above by what the replica keeps, not by what the venue will send:
	 * @c depth_reconstructor holds a default-constructed @c l2_book, which
	 * retains @c l2_book::DEFAULT_DEPTH (128) levels a side and discards the
	 * rest on @c load. Asking for more than that fetches and parses depth only
	 * to drop it. The default sits under the ceiling deliberately; a deeper
	 * replica needs the book sized, which is a change to @c
	 * reconstructor_options rather than a bigger number here.
	 */
	int limit          = 100;
	int price_decimals = 2; ///< Tick precision for the listing.
	int qty_decimals   = 2; ///< Step precision for the listing.
	/// @brief Diff-stream cadence.
	market_data::binance::depth_speed speed =
		market_data::binance::depth_speed::every_100ms;

	/**
	 * @brief Certificate policy for the depth stream's TLS connection.
	 *
	 * Verified by default, which is the stronger requirement it looks like:
	 * this feed is the input every quote and every order downstream is derived
	 * from, so an unverified stream lets whoever terminates the connection
	 * decide the book this process believes in. That is a worse outcome than
	 * the leak an unverified *credential* would cause, and the credentialed
	 * path already defaults this way.
	 *
	 * @c transport::tls_verify::none exists for a host with no CA bundle
	 * installed, where the handshake would otherwise fail outright. It is the
	 * operator's call and it is spelled out at the command line (@c
	 * --insecure-tls), never inferred from a failure.
	 */
	transport::tls_verify verify = transport::tls_verify::peer;

	/**
	 * @brief Which deployment of the venue to read depth from.
	 *
	 * Must be whatever the order path is pointed at, and that is a correctness
	 * requirement rather than tidiness: testnet keeps its own book and its own
	 * liquidity, so a run reading production depth while placing orders there
	 * is a strategy reacting to a market it is not trading in. @c host_for is
	 * the one table both directions read. @see venue::environment
	 */
	venue::environment env = venue::environment::production;

	/// @brief Pause before rebuilding a dropped stream. Not zero: a venue that
	///        just closed on us is not helped by an immediate retry, and a
	///        tight reconnect loop against a rate-limited endpoint gets an
	///        address banned rather than connected.
	std::chrono::milliseconds reconnect_delay{500};
	/// @brief Reconnect attempts before giving up; zero means keep trying.
	std::size_t max_reconnects = 0;

	/**
	 * @brief Floor on the gap between snapshot fetches after one fails.
	 *
	 * The stream has a reconnect delay for exactly this reason and the REST
	 * half had none, which is the more dangerous of the two: the frame loop
	 * asks for a snapshot whenever the replica still wants one, so a failing
	 * fetch used to be retried on the very next frame - ten a second at the
	 * default cadence, five weight each against a 6000-per-minute IP budget.
	 * That is half the budget spent re-learning one refusal, and Binance's
	 * documented answer to it is a 429 and then a 418 ban on the address.
	 *
	 * A server-supplied @c Retry-After overrides this when it is *longer*.
	 * Never when it is shorter: the venue setting a small value is not a reason
	 * to ignore our own floor.
	 */
	std::chrono::milliseconds snapshot_retry_delay{1000};
};

/// @brief What a live run did.
struct live_feed_report {
	std::uint64_t frames              = 0; ///< Frames decoded and handed over.
	std::uint64_t malformed           = 0; ///< Frames that would not decode.
	std::uint64_t snapshots_requested = 0; ///< REST fetches started.
	std::uint64_t snapshots_applied   = 0; ///< Fetches that reached the book.
	std::uint64_t snapshots_failed    = 0; ///< Fetches that did not.
	std::uint64_t reconnects          = 0; ///< Streams rebuilt after a drop.
	std::string stopped; ///< Why the run ended. Never empty on return.
};

namespace detail {

/// The completion token this file awaits with.
///
/// transport keeps its own deliberately private, so this is the same choice
/// made again rather than shared: @c as_tuple delivers each completion as a
/// tuple led by the error code, so a closed channel or a cancelled timer is a
/// value to inspect and not an exception thrown across a @c co_await.
inline constexpr auto TOKEN = boost::asio::as_tuple(boost::asio::use_awaitable);

/**
 * @brief Why a snapshot fetch produced nothing, and what to do about it.
 *
 * A string would do for the log line and would be wrong for the decision. The
 * frame loop re-asks for a snapshot as soon as the replica still wants one, so
 * on a failing fetch the *only* thing standing between this process and one
 * request per frame is what this type carries. At a 100ms cadence that is ten
 * requests a second at five weight each - half of Binance's documented
 * per-minute IP budget spent learning the same refusal over and over, and the
 * documented route from a 429 to a 418 address ban.
 */
struct snapshot_failure {
	std::string reason{}; ///< for the log line

	/// @brief What the venue asked us to wait, when it said. @see rest::failure
	std::optional<std::chrono::seconds> retry_after{};

	/// @brief Whether the identical request could ever succeed. False for a bad
	///        symbol or a malformed query, where retrying spends rate-limit
	///        budget to learn nothing.
	bool is_retryable = true;
};

/// @brief What a snapshot fetch produced: the depth, or why there wasn't any.
///
/// The failure travels with the result rather than being reported by the fetch
/// itself, because reporting it would mean calling @c snapshot_failed on the
/// handler - and the handler has one writer, which is not this chain.
using snapshot_result =
	std::expected<market_data::book_snapshot, snapshot_failure>;

/// @brief The hand-off between the fetch chain and the frame loop.
///
/// Capacity one, which is all that can ever be needed: the frame loop starts at
/// most one fetch at a time and drains the channel before starting another.
using snapshot_channel = boost::asio::experimental::channel<void(
	boost::system::error_code, snapshot_result)>;

/**
 * @brief Fetch a REST snapshot and post the outcome to @p channel.
 *
 * Spawned detached so it overlaps the frame loop rather than blocking it. It
 * captures the channel by @c shared_ptr and captures nothing else - no handler,
 * no pointer into the pipeline's coroutine frame - so it is safe however and
 * whenever the pipeline ends. A pipeline that has gone away leaves a closed
 * channel behind, the send fails immediately, and the result is dropped, which
 * is the correct thing to do with a snapshot nobody is waiting for.
 */
inline boost::asio::awaitable<void>
fetch_snapshot(std::string symbol, live_feed_options options,
			   std::shared_ptr<snapshot_channel> channel) {
	namespace binance = market_data::binance;

	auto [host, target] =
		binance::depth_snapshot_endpoint(symbol, options.limit, options.env);
	snapshot_result result =
		std::unexpected(snapshot_failure{.reason = "not fetched"});

	auto fetched =
		co_await transport::rest::https_get(std::move(host), std::move(target));
	// Before the parse, for the same reason the frame path stamps before the
	// decode: the fetch is what took the time, and folding our own JSON pass
	// into the arrival stamp would hide it.
	const auto arrived = core::chrono::ingress_clock::now();
	if (!fetched) {
		const transport::rest::failure &why = fetched.error();
		// The venue's own words where it gave any: "Invalid symbol." beats
		// "HTTP 400: {json}" for whoever has to fix it. Falls back to the
		// status line when the body is not an error envelope. @see
		// venue::binance::parse_api_error
		result = std::unexpected(snapshot_failure{
			.reason =
				venue::binance::describe_api_error(why.body, why.message()),
			.retry_after  = why.retry_after,
			.is_retryable = why.is_retryable()});
	} else {
		auto parsed = binance::parse_binance_depth(fetched->body,
												   options.price_decimals,
												   options.qty_decimals);
		if (!parsed)
			result = std::unexpected(
				snapshot_failure{.reason = binance::message(parsed.error()),
								 .is_retryable = true});
		else {
			market_data::book_snapshot normalised =
				binance::normalise(std::move(*parsed));
			normalised.ingress = arrived;
			result             = std::move(normalised);
		}
	}

	[[maybe_unused]] auto [ignored] =
		co_await channel->async_send(boost::system::error_code{},
									 std::move(result),
									 TOKEN);
}

/**
 * @brief One listing's live pipeline: the stream, the decoder, the replica's
 *        sole writer, and the hand-off the fetches post into.
 *
 * A class rather than one long coroutine because it has genuine state -
 * a stream that survives reconnects, a decoder whose buffers amortise, a
 * channel, and the counters - and because the frame loop reads as five short
 * steps once each of them is named.
 *
 * @note Not thread-safe and single-consumer by construction: one stream has one
 *       frame order, and this object is the replica's only writer.
 */
template <live_handler Handler>
class live_pipeline {
public:
	/**
	 * @brief Build the pipeline for @p symbol. Nothing is opened until @c run.
	 *
	 * @param symbol Trading pair, e.g. @c SOLUSDT.
	 * @param handler The replica; must outlive this object.
	 * @param options Cadence, precision, duration and reconnect policy.
	 */
	live_pipeline(std::string symbol, Handler &handler,
				  live_feed_options options)
		: symbol_(std::move(symbol)),
		  handler_(&handler),
		  options_(options),
		  reader_(endpoint_of(symbol_, options.speed, options.verify,
							  options.env)),
		  decoder_(options.price_decimals, options.qty_decimals) {}

	/// @brief Subscribe, then run until the duration elapses or the stream is
	///        lost beyond recovery.
	boost::asio::awaitable<live_feed_report> run() {
		snapshots_ = std::make_shared<snapshot_channel>(
			co_await boost::asio::this_coro::executor,
			1);

		if (const auto opened = co_await reader_.connect(); !opened) {
			report_.stopped = fmt::format("connect: {}", opened.error());
			co_return report_;
		}
		spdlog::info("live feed subscribed to {} @{}", symbol_, options_.speed);
		request_snapshot_if_needed();

		const auto deadline = deadline_of(options_.duration);
		while (report_.stopped.empty()) {
			if (deadline && std::chrono::steady_clock::now() >= *deadline) {
				report_.stopped = "duration elapsed";
				break;
			}

			const auto frame = co_await reader_.read();
			// Stamped here and nowhere later: this is the first instruction of
			// ours that runs after the read completed, so it is the closest a
			// portable path gets to when the bytes arrived. What is still
			// outside the stamp is the kernel's receive path and Beast's
			// framing, neither of which a steady_clock can see - a venue where
			// that matters wants the NIC's own stamp, which the DPDK path
			// already takes into transport::packet_view.
			// @see core::chrono::ingress_clock
			const auto arrived = core::chrono::ingress_clock::now();
			if (!frame) {
				if (!co_await handle_drop(frame.error())) break;
				continue;
			}
			on_frame(*frame, arrived);
		}

		co_await reader_.close();
		drain_snapshot(); // whatever landed during the final frame

		// Closing releases a fetch still waiting to send: its async_send
		// completes immediately with an error, the coroutine unwinds, and the
		// last reference to the channel goes with it. Nothing here waits for
		// that, because nothing here is what it would be waiting on - the fetch
		// holds no pointer into this object. The io_context outlives us and
		// will finish it.
		snapshots_->close();
		co_return report_;
	}

private:
	/// Resolve the venue's stream endpoint. Static so it can run in the member
	/// initialiser list, where @c reader_ is built.
	static transport::ws::stream_reader
	endpoint_of(const std::string &symbol,
				market_data::binance::depth_speed speed,
				transport::tls_verify verify, venue::environment env) {
		auto endpoint =
			market_data::binance::diff_depth_stream(symbol, speed, env);
		return {std::move(endpoint.host),
				std::move(endpoint.port),
				std::move(endpoint.target),
				verify};
	}

	static std::optional<std::chrono::steady_clock::time_point>
	deadline_of(std::chrono::seconds duration) {
		if (duration.count() <= 0) return std::nullopt;
		return std::chrono::steady_clock::now() + duration;
	}

	boost::asio::awaitable<void> sleep_for(std::chrono::milliseconds how_long) {
		boost::asio::steady_timer timer(
			co_await boost::asio::this_coro::executor);
		timer.expires_after(how_long);
		[[maybe_unused]] auto [ignored] = co_await timer.async_wait(TOKEN);
	}

	/// Decode one frame into the replica, then service the snapshot half.
	/// @param frame The raw @c depthUpdate JSON.
	/// @param arrived When the read that produced it completed. Travels onto
	/// the
	///        event so whoever reacts to it can say how late it was.
	void on_frame(std::string_view frame, core::chrono::ingress_time arrived) {
		auto event = decoder_.decode(frame, reader_.frames(), arrived);
		if (!event) {
			// One unreadable frame is a sequence gap and nothing more: the
			// sequencer will see the discontinuity in the next frame that does
			// decode and force a resync through the ordinary path. Stopping the
			// pipeline over it would turn a recoverable blip into an outage.
			++report_.malformed;
			spdlog::warn("frame {} did not decode: {}",
						 reader_.frames(),
						 event.error().detail);
			return;
		}

		++report_.frames;
		handler_->on_event(std::move(*event));
		// After the event, so a snapshot and the frame that may bridge it reach
		// the reconstructor in the order they arrived.
		drain_snapshot();
		request_snapshot_if_needed();
	}

	/**
	 * @brief Apply a finished fetch, if one is waiting.
	 *
	 * Synchronous and non-blocking: @c try_receive runs the handler inline when
	 * a value is ready and does nothing when it is not, so the frame loop never
	 * has to race a pending receive against a pending read - which matters,
	 * because cancelling a WebSocket read mid-frame is not something a stream
	 * recovers from.
	 */
	void drain_snapshot() {
		snapshots_->try_receive([this](const boost::system::error_code &ec,
									   snapshot_result result) {
			if (ec) return;
			fetching_ = false;
			if (result) {
				// The reconstructor judges what this is worth: it drops
				// buffered events the snapshot already covers, refuses one
				// that predates them, and ignores one that would move a live
				// replica backwards. None of that is decided here.
				handler_->on_snapshot(std::move(*result));
				++report_.snapshots_applied;
				return;
			}
			const snapshot_failure &why = result.error();
			// Hold off before asking again, and say for how long. Without this
			// the frame loop re-requests on the next frame, which is how a
			// throttled client becomes a banned one.
			// @see live_feed_options::snapshot_retry_delay
			const auto wait = std::max(
				options_.snapshot_retry_delay,
				why.retry_after
					? std::chrono::duration_cast<std::chrono::milliseconds>(
						  *why.retry_after)
					: std::chrono::milliseconds::zero());
			retry_snapshot_after_ = std::chrono::steady_clock::now() + wait;

			if (!why.is_retryable)
				// Still throttled rather than abandoned: the replica decides
				// whether it wants another snapshot, and a listing that is
				// wrong now stays wrong, so the useful behaviour is to keep
				// saying so slowly rather than to spin or to exit from here.
				spdlog::error(
					"snapshot fetch refused permanently: {} - retrying in {} "
					"anyway, but this will not fix itself",
					why.reason,
					wait);
			else
				spdlog::warn("snapshot fetch failed: {} - next attempt in {}",
							 why.reason,
							 wait);

			handler_->snapshot_failed();
			++report_.snapshots_failed;
		});
	}

	/**
	 * @brief Start a fetch if the replica wants one and none is outstanding.
	 *
	 * The second condition is not redundant with the first. @c needs_snapshot
	 * already goes false while a fetch is announced, but @c invalidate clears
	 * that announcement deliberately - an explicit invalidate means the world
	 * changed under the in-flight request - and the request itself is still
	 * running. Without this guard a reconnect would spawn a duplicate.
	 */
	void request_snapshot_if_needed() {
		if (fetching_ || !handler_->needs_snapshot()) return;
		// Third condition, and the one that keeps this loop off a rate limit. A
		// steady clock rather than the venue's: this measures an interval, and
		// a venue timestamp that steps would turn the interval negative and let
		// the retry through immediately. @see
		// live_feed_options::snapshot_retry_delay
		if (std::chrono::steady_clock::now() < retry_snapshot_after_) return;
		fetching_ = true;
		handler_->snapshot_requested();
		++report_.snapshots_requested;
		boost::asio::co_spawn(snapshots_->get_executor(),
							  fetch_snapshot(symbol_, options_, snapshots_),
							  boost::asio::detached);
	}

	/**
	 * @brief Rebuild a dropped stream, or declare the run over.
	 *
	 * Whatever the venue published while we were not listening is gone, and
	 * absolute level sizes leave no trace of the hole - so the replica is stale
	 * by construction and has to say so before anything else happens. The
	 * snapshot that repairs it is requested only once the stream is back, for
	 * the same reason it is on the first connect: subscribe, then snapshot.
	 * @param status Why the read failed; taken by value, as a coroutine
	 *        parameter must be.
	 * @return @c false when the run should stop; @c report_.stopped says why.
	 */
	boost::asio::awaitable<bool>
	handle_drop(transport::ws::stream_status status) {
		handler_->invalidate();
		const std::string why =
			is_closed(status) ? std::string("closed by peer") : status.detail;

		if (options_.max_reconnects != 0 &&
			reconnects_ >= options_.max_reconnects) {
			report_.stopped = fmt::format("stream lost after {} reconnects: {}",
										  reconnects_,
										  why);
			co_return false;
		}

		++reconnects_;
		++report_.reconnects;
		spdlog::warn("stream lost ({}); reconnecting in {}",
					 why,
					 options_.reconnect_delay);
		co_await sleep_for(options_.reconnect_delay);

		if (const auto reopened = co_await reader_.connect(); !reopened) {
			spdlog::warn("reconnect failed: {}", reopened.error());
			co_return true; // counted, delayed, and tried again next round
		}
		drain_snapshot();
		request_snapshot_if_needed();
		co_return true;
	}

	std::string symbol_;
	Handler *handler_;
	live_feed_options options_;
	transport::ws::stream_reader reader_;
	market_data::binance::depth_frame_decoder decoder_;
	/// Built in run(), which is the first place an executor is available.
	std::shared_ptr<snapshot_channel> snapshots_;
	/// Owned by the frame loop alone - the fetch chain has no idea it exists.
	bool fetching_ = false;
	/// Earliest a new fetch may start. Frame-loop-local for the same reason
	/// `fetching_` is: it is a decision about this loop's own pacing, so there
	/// is nothing to share and nothing to race over. Epoch-default lets the
	/// first fetch through without a special case.
	std::chrono::steady_clock::time_point retry_snapshot_after_;
	std::size_t reconnects_ = 0;
	live_feed_report report_;
};

} // namespace detail

/**
 * @brief Run the live ingest pipeline for one listing into @p handler.
 *
 * @code
 * market_data::depth_reconstructor replica;
 * const auto report = co_await run_live_feed("SOLUSDT", &replica, {});
 * @endcode
 *
 * @par The order of operations, which is the procedure
 * Subscribe first, then snapshot. The stream is connected before any fetch is
 * issued, so the events that arrive during the fetch are buffered rather than
 * lost, and one of them is the event that bridges the snapshot to the present.
 * Fetching first and subscribing second leaves a hole between the two that
 * nothing can close.
 *
 * @param symbol Trading pair, e.g. @c SOLUSDT.
 * @param handler The replica to drive; never null. This coroutine is its only
 *        writer, and the replica must outlive the whole coroutine rather than
 *        just this call - a coroutine's parameters are not lifetime-extended.
 *        Spelled as a pointer for exactly that reason: @c &replica at the call
 *        site is the reminder that an obligation is being taken on.
 * @param options Cadence, precision, duration and reconnect policy.
 * @return What the run did, and why it stopped.
 *
 * @note Single-threaded is no longer a correctness requirement of the replica -
 *       it has one writer either way - but the io_context should still be run
 *       on one thread, because @p handler is not thread-safe and this coroutine
 *       may otherwise be resumed on a different one than it suspended on.
 */
template <live_handler Handler>
boost::asio::awaitable<live_feed_report>
run_live_feed(std::string symbol, Handler *handler,
			  live_feed_options options = {}) {
	detail::live_pipeline<Handler> pipeline(std::move(symbol),
											*handler,
											options);
	co_return co_await pipeline.run();
}

} // namespace exchange::session
