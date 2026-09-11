#pragma once
// The return leg, running: the venue's account stream, kept alive, and what to
// do when it is not.
//
// `live_feed.hpp` is this file's opposite number - the same three problems for
// market data, solved the same way, and worth reading first. What differs is
// what a gap *means*. A dropped depth stream loses public information that the
// next snapshot restores in full. A dropped user data stream loses reports
// about our own orders, and nothing replays them: a fill that happened while
// the socket was down happened, and the only way to learn of it is to ask what
// is working now. That is why this reports a gap rather than swallowing it, and
// why `stale` is a state a caller has to clear rather than a log line.
//
// --- the three failure modes, and which are handled here --------------------
//
// The socket drops - reconnect, re-subscribe, and report the gap.
// The subscribe is refused - a credential or a permissions problem, and fatal:
//   a socket nobody will send events on is not a feed, and retrying it would
//   turn one wrong answer into a loop of them. The venue's own words go into
//   the report, because the enum deliberately does not carry them.
//
// The third failure the listen-key flow had - a keepalive quietly lapsing and
// the socket ending half an hour later with no error - no longer exists. There
// is no key with a life of its own any more; the subscription lives exactly as
// long as the connection, so a lapse and a drop are the same event and have one
// handler. @see venue/binance/user_data.hpp

#include "core/logging.hpp"
#include "session/venue_bridge.hpp"
#include "transport/tls_verify.hpp"
#include "transport/websocket.hpp"
#include "venue/binance/user_data.hpp"
#include "venue/credentials.hpp"
#include "venue/environment.hpp"
#include "venue/execution_report.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::session {

/**
 * @brief What a caller must be able to do with the account stream's output.
 *
 * @note @c on_gap is not optional and not a courtesy. It is the one thing a
 *       consumer cannot work out for itself: from a sequence of reports alone,
 *       a stream that went quiet because nothing happened is identical to one
 *       that went quiet because the socket died.
 */
template <typename H>
concept user_data_handler =
	requires(H &handler, const venue::execution_report &report,
			 std::string_view reason) {
		{ handler.on_report(report) } -> std::same_as<void>;
		{ handler.on_gap(reason) } -> std::same_as<void>;
	};

/// @brief How a @ref user_data_pipeline behaves.
struct user_data_options {
	/// @brief Which deployment. Must match whatever the order path is pointed
	///        at - testnet keeps its own orders. @see venue::host_for
	venue::environment env = venue::environment::testnet;

	/// @brief Price scale for the listing's reports. @see symbol_filters
	int price_decimals = 2;

	/// @brief Size scale. Frequently not equal to @c price_decimals.
	int qty_decimals = 3;

	/// @brief How long to run; zero runs until the stream ends or the caller
	///        cancels.
	std::chrono::seconds duration{0};

	/// @brief Pause before rebuilding a dropped stream. @see live_feed_options
	std::chrono::milliseconds reconnect_delay{500};

	/// @brief Reconnects to attempt before giving up; zero is unlimited.
	std::uint32_t max_reconnects = 0;

	/// @brief Certificate policy. Peer by default - this stream is keyed by a
	///        bearer credential in its URL. @see tls_verify
	transport::tls_verify verify = transport::tls_verify::peer;
};

/// @brief What a run of the account stream did.
struct user_data_report {
	std::uint64_t frames       = 0; ///< frames read off the socket
	std::uint64_t reports      = 0; ///< execution reports decoded from them
	std::uint64_t other_events = 0; ///< balance updates and the like, skipped
	std::uint64_t malformed    = 0; ///< frames that would not decode
	std::uint64_t reconnects   = 0; ///< streams rebuilt after a drop
	std::uint64_t subscribes   = 0; ///< subscribe requests the venue confirmed
	std::string stopped{};                ///< why the run ended
};

/**
 * @brief Runs the venue's account stream for one credential.
 *
 * @tparam Handler What to do with each report, and with each gap.
 *
 * @note One listing's scales, one handler, one socket. A process trading two
 *       listings runs two of these - the stream itself is per *account* and
 *       carries both, but the scales needed to decode a report are per listing,
 *       and guessing which listing a frame belongs to before decoding it is not
 *       a guess worth making.
 */
template <user_data_handler Handler>
class user_data_pipeline {
public:
	user_data_pipeline(venue::credentials creds, Handler *handler,
					   user_data_options options)
		: creds_(std::move(creds)), handler_(handler), options_(options) {}

	/**
	 * @brief Open the stream and pump it until it ends.
	 *
	 * @return What the run did, including why it stopped.
	 *
	 * @note Never throws out of the coroutine: a transport failure is a value
	 *       on the way to the report, and a run that could not start reports
	 *       that rather than unwinding through a @c co_await.
	 */
	[[nodiscard]] boost::asio::awaitable<user_data_report> run() {
		if (creds_.key.empty()) {
			report_.stopped = "no API key; the account stream needs one";
			co_return report_;
		}

		const auto deadline = deadline_from(options_.duration);
		while (true) {
			if (!co_await open()) break;
			// Every reconnect is a gap in *our own* order history, not merely a
			// reconnect. Said before the first frame of the new stream so a
			// consumer reconciles before acting on it.
			if (report_.reconnects > 0)
				handler_->on_gap("the account stream was rebuilt; reports "
								 "between the drop and now were not delivered");

			if (!co_await pump(deadline)) break;
			if (!co_await retry()) break;
		}
		co_await shut_down();
		co_return report_;
	}

private:
	using clock = std::chrono::steady_clock;

	/// Frames to read while waiting for the subscribe response before giving
	/// up. Not one: the venue may push an event between the request and its
	/// answer, and those are decoded rather than discarded. Bounded so a socket
	/// that answers everything except this cannot hold the loop forever.
	static constexpr int MAX_FRAMES_BEFORE_REPLY = 32;

	/// Wall-clock milliseconds, for the venue's replay window. Wall rather than
	/// steady because it is a statement the *venue* has to agree with, which is
	/// the same split `core/chrono/wall.hpp` argues for the lifecycle records.
	[[nodiscard]] static std::int64_t wall_now_ms() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::system_clock::now().time_since_epoch())
			.count();
	}

	[[nodiscard]] static clock::time_point
	deadline_from(std::chrono::seconds duration) {
		return duration.count() == 0 ? clock::time_point::max()
									 : clock::now() + duration;
	}

	/// The socket, and the subscribe request that turns it into a feed.
	///
	/// Three steps where the listen-key flow had two, and the middle one is the
	/// whole difference: this socket answers requests, so it sends nothing
	/// until it has been asked to. @see venue::binance::subscribe_request
	[[nodiscard]] boost::asio::awaitable<bool> open() {
		const auto endpoint = venue::binance::ws_api_endpoint(options_.env);
		// Safe to log, unlike the endpoint a listen key produced: the
		// credential is in the request below, not in this URL.
		spdlog::info("account stream: connecting to {}{}",
					 endpoint.host,
					 endpoint.target);

		reader_.emplace(endpoint.host,
						endpoint.port,
						endpoint.target,
						options_.verify);
		if (const auto opened = co_await reader_->connect(); !opened) {
			report_.stopped =
				fmt::format("account stream refused: {}", opened.error());
			co_return false;
		}
		co_return co_await subscribe();
	}

	/// Ask for the account's events, and read the answer.
	[[nodiscard]] boost::asio::awaitable<bool> subscribe() {
		// A fresh id per attempt, so a reply from a previous subscription on a
		// reused socket cannot be read as this one's.
		++request_seq_;
		const std::string id = fmt::format("uds-{}", request_seq_);
		const auto request =
			venue::binance::subscribe_request(creds_, wall_now_ms(), id);
		if (!request) {
			report_.stopped = std::string(message(request.error()));
			co_return false;
		}

		if (const auto sent = co_await reader_->send(*request); !sent) {
			report_.stopped = fmt::format("could not ask for the account "
										  "stream: {}",
										  sent.error());
			co_return false;
		}

		// Read until the answer arrives rather than assuming it is first. The
		// venue may push an event between the request and its response, and a
		// reader that treated the first frame as the answer would discard a
		// real execution report to do it.
		for (int frame_no = 0; frame_no < MAX_FRAMES_BEFORE_REPLY; ++frame_no) {
			const auto frame = co_await reader_->read();
			if (!frame) {
				const transport::ws::stream_status &why = frame.error();
				report_.stopped =
					fmt::format("the account stream closed before confirming "
								"the subscription: {}",
								is_closed(why) ? std::string("closed by peer")
											   : why.detail);
				co_return false;
			}
			++report_.frames;
			if (venue::binance::is_user_data_event(*frame)) {
				decode(*frame);
				continue;
			}

			const auto confirmed =
				venue::binance::parse_subscribe_reply(*frame);
			if (!confirmed) {
				// The venue's own words, which the enum deliberately does not
				// carry: a refusal here is a credential or a permissions
				// problem and the message is the whole diagnosis.
				report_.stopped =
					fmt::format("the venue declined the account stream ({}): {}",
								message(confirmed.error()),
								*frame);
				co_return false;
			}
			subscription_ = *confirmed;
			++report_.subscribes;
			spdlog::info("account stream: subscribed (subscription {})",
						 subscription_);
			co_return true;
		}

		report_.stopped = "the venue never answered the subscription request";
		co_return false;
	}

	/// Read frames until the socket stops or the deadline passes.
	/// @return Whether a reconnect should be attempted.
	[[nodiscard]] boost::asio::awaitable<bool>
	pump(clock::time_point deadline) {
		while (true) {
			if (clock::now() >= deadline) {
				report_.stopped = "the configured duration elapsed";
				co_return false;
			}

			const auto frame = co_await reader_->read();
			if (!frame) {
				// The same two-way reading `live_feed::handle_drop` makes, and
				// for the same reason: a close is the venue ending the
				// conversation and carries no detail, while a failure carries
				// the transport's own words and is the one worth quoting.
				const transport::ws::stream_status &why = frame.error();
				spdlog::warn("account stream stopped: {}",
							 is_closed(why) ? std::string("closed by peer")
											: why.detail);
				co_return true;
			}
			++report_.frames;
			decode(*frame);
		}
	}

	/// Decode one frame, counting what it turned out to be.
	void decode(std::string_view frame) {
		// One level down, because the WebSocket API wraps a push as
		// {"subscriptionId":N,"event":{...}}. Permissive about the wrapper
		// being absent, so this same decoder still reads a frame from the old
		// listen-key stream and every test that hands it one directly.
		// @see venue::binance::unwrap_event
		const auto decoded = venue::binance::parse_execution_report(
			venue::binance::unwrap_event(frame),
			options_.price_decimals,
			options_.qty_decimals);
		if (decoded) {
			++report_.reports;
			handler_->on_report(*decoded);
			return;
		}
		// A balance update is a valid frame that is simply not this one. Only
		// a frame that would not decode at all is worth counting as malformed.
		if (decoded.error() ==
			venue::binance::user_data_error::not_an_execution_report) {
			++report_.other_events;
			return;
		}
		++report_.malformed;
		spdlog::warn("account stream: undecodable frame ({})",
					 message(decoded.error()));
	}

	// There is no keepalive any more, and its absence is the change rather
	// than an omission. A listen key was a credential with a 60-minute life
	// that had to be renewed against the clock; a subscription on the
	// WebSocket API lives exactly as long as its socket, so the thing that
	// keeps it alive is the connection, and the thing that ends it is a drop -
	// which `retry` below already handles. @see venue/binance/user_data.hpp

	/// Wait, then decide whether another attempt is allowed.
	[[nodiscard]] boost::asio::awaitable<bool> retry() {
		if (options_.max_reconnects != 0 &&
			report_.reconnects >= options_.max_reconnects) {
			report_.stopped = fmt::format("stream lost after {} reconnects",
										  report_.reconnects);
			co_return false;
		}
		++report_.reconnects;

		boost::asio::steady_timer timer(
			co_await boost::asio::this_coro::executor);
		timer.expires_after(options_.reconnect_delay);
		boost::system::error_code ignored;
		co_await timer.async_wait(
			boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
		co_return true;
	}

	/// Close the socket.
	///
	/// Shorter than it was, and for a good reason: there is no listen key to
	/// hand back. The old flow had to release one explicitly because an
	/// abandoned key held one of the account's limited stream slots until it
	/// expired. A subscription here is owned by the socket, so closing the
	/// socket is the whole of it.
	boost::asio::awaitable<void> shut_down() {
		if (reader_) co_await reader_->close();
	}

	venue::credentials creds_;
	Handler *handler_;
	user_data_options options_;

	std::optional<transport::ws::stream_reader> reader_;

	/// The venue's handle for our subscription. Zero is a valid one - it is an
	/// index and the first is the first - so it is not a "not subscribed" flag.
	std::int64_t subscription_ = 0;

	/// Distinguishes one subscribe request from the next. @see subscribe
	std::uint64_t request_seq_ = 0;

	user_data_report report_{};
};

/**
 * @brief Run the account stream for @p creds until it ends.
 *
 * The one-shot form, matching @c run_live_feed. Use @ref user_data_pipeline
 * directly when the stream should outlive one call.
 */
template <user_data_handler Handler>
[[nodiscard]] boost::asio::awaitable<user_data_report>
run_user_data_feed(venue::credentials creds, Handler *handler,
				   user_data_options options = {}) {
	user_data_pipeline<Handler> pipeline(std::move(creds), handler, options);
	co_return co_await pipeline.run();
}

} // namespace exchange::session
