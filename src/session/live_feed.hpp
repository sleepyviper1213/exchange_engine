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
#include "market-data/binance/binance_depth.hpp"
#include "market-data/binance/depth_feed.hpp" // depth_frame_decoder
#include "market-data/binance/depth_speed.hpp"
#include "market-data/binance/endpoints.hpp"
#include "market-data/binance/normalise.hpp"
#include "market-data/feed.hpp"
#include "market-data/normalised.hpp"
#include "transport/rest.hpp"
#include "transport/websocket.hpp"

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
#include <fmt/format.h>

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
	/// @brief Pause before rebuilding a dropped stream. Not zero: a venue that
	///        just closed on us is not helped by an immediate retry, and a
	///        tight reconnect loop against a rate-limited endpoint gets an
	///        address banned rather than connected.
	std::chrono::milliseconds reconnect_delay{500};
	/// @brief Reconnect attempts before giving up; zero means keep trying.
	std::size_t max_reconnects = 0;
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
inline constexpr auto kToken =
	boost::asio::as_tuple(boost::asio::use_awaitable);

/// @brief What a snapshot fetch produced: the depth, or why there wasn't any.
///
/// The failure travels with the result rather than being reported by the fetch
/// itself, because reporting it would mean calling @c snapshot_failed on the
/// handler - and the handler has one writer, which is not this chain.
using snapshot_result = std::expected<market_data::book_snapshot, std::string>;

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

	auto [host, target]    = binance::depth_snapshot(symbol, options.limit);
	snapshot_result result = std::unexpected("not fetched");

	auto body =
		co_await transport::rest::https_get(std::move(host), std::move(target));
	if (!body) {
		result = std::unexpected(body.error());
	} else {
		auto parsed = binance::parse_binance_depth(*body,
												   options.price_decimals,
												   options.qty_decimals);
		if (!parsed) result = std::unexpected(binance::message(parsed.error()));
		else result = binance::normalise(std::move(*parsed));
	}

	auto [ignored] = co_await channel->async_send(boost::system::error_code{},
												  std::move(result),
												  kToken);
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
		  reader_(endpoint_of(symbol_, options.speed)),
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
			if (!frame) {
				if (!co_await handle_drop(frame.error())) break;
				continue;
			}
			on_frame(*frame);
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
				market_data::binance::depth_speed speed) {
		auto endpoint = market_data::binance::diff_depth_stream(symbol, speed);
		return {std::move(endpoint.host),
				std::move(endpoint.port),
				std::move(endpoint.target)};
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
		auto [ignored] = co_await timer.async_wait(kToken);
	}

	/// Decode one frame into the replica, then service the snapshot half.
	void on_frame(std::string_view frame) {
		auto event = decoder_.decode(frame, reader_.frames());
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
			spdlog::warn("snapshot fetch failed: {}", result.error());
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
	bool fetching_          = false;
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
