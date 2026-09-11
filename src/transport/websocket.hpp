#pragma once

// WebSocket transport: a live text feed, read one frame at a time, plus the
// capture path that writes those frames to a JSONL file for offline replay
// (see transport/replay.hpp and benchmark/market_replay.cpp).
//
// Deliberately venue-agnostic. This layer opens a socket, upgrades it, and
// hands over frames; it does not know that the frames are Binance `depthUpdate`
// diffs, or that they aggregate by price. Which endpoint to point it at is
// market data's knowledge: call market_data::binance::diff_depth_stream() for a
// resolved {host, port, target} and pass that here. Keeping the venue's stream
// grammar out of transport is the same boundary that keeps the published-depth
// book (market_data::l2_book) out of the matching engine.
//
// Link OpenSSL + Boost (see transport/CMakeLists).

#include "core/util/indirect.hpp"
#include "tls_verify.hpp"       // IWYU pragma: export - a constructor parameter
#include "transport_export.hpp" // TRANSPORT_EXPORT (generated)

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace exchange::transport::ws {

/// @brief Why a frame read produced no frame.
enum class stream_stop : std::uint8_t {
	closed, ///< The peer closed the connection - an ending, not a fault.
	failed, ///< Anything else: a read error, a timeout, a broken transport.
};

/**
 * @brief Why a read stopped, and what the transport said about it.
 *
 * The two are kept apart because a caller reacts to them differently: a close
 * is the venue ending the conversation and the answer is to reconnect, while a
 * failure may well be the same thing about to happen again. Collapsing both
 * into an error string would leave every caller pattern-matching on prose.
 */
struct stream_status {
	stream_stop reason = stream_stop::closed;
	std::string detail; ///< Transport's own message; empty for a clean close.
};

/// @brief Whether the connection ended rather than broke.
///
/// Free, because @c stream_status is a reason and a message a caller fills in,
/// not a type with something to protect. @see stream_stop
[[nodiscard]] inline bool is_closed(const stream_status &status) noexcept {
	return status.reason == stream_stop::closed;
}

/**
 * @brief A live text WebSocket, pulled one frame at a time.
 *
 * @code
 * ws::stream_reader reader(host, port, target);
 * if (const auto opened = co_await reader.connect(); !opened) return;
 * while (const auto frame = co_await reader.read())
 *     decode(*frame);
 * @endcode
 *
 * @par Why a reader rather than a callback
 * A callback would own the loop, and the whole point of a live ingest pipeline
 * is that the frame loop is where the *other* asynchronous work is coordinated
 * from - a snapshot fetch launched because the last frame revealed a gap.
 * Inverting that puts the coordination inside a callback that cannot suspend.
 * Pulling also makes this the asynchronous mirror of
 * @c market_data::depth_feed::next(), so an offline driver and a live one read
 * the same way and differ only in what they @c co_await.
 *
 * @note Not thread-safe, and not merely by convention: a WebSocket stream
 *       permits one read at a time, and a second concurrent @c read() would
 *       interleave two halves of one frame. One reader, one consuming
 *       coroutine.
 */
class stream_reader {
public:
	/**
	 * @brief A reader for one endpoint. Nothing is opened until @c connect.
	 * @param host Endpoint host; also the SNI, and what the certificate is
	 *        matched against when @p verify is @c tls_verify::peer.
	 * @param port Endpoint port.
	 * @param target Stream path.
	 * @param verify Certificate policy, applied by every @c connect. Defaults
	 *        to @c tls_verify::peer, which is a change from the hardcoded
	 *        @c none this reader used to carry: a feed is not a secret but it
	 *        *is* what the strategy above decides from, so an unverified one
	 *        lets whoever terminates the connection dictate the book this
	 *        process believes in. @see tls_verify
	 * @note Take the three endpoint fields from a market_data endpoint
	 *       descriptor (e.g. @c binance::diff_depth_stream) rather than
	 *       spelling a venue's stream name at the call site.
	 */
	TRANSPORT_EXPORT stream_reader(std::string host, std::string port,
								   std::string target,
								   tls_verify verify = tls_verify::peer);
	TRANSPORT_EXPORT ~stream_reader();
	/**
	 * @brief Not copyable
	 * Because a second reader on one socket is not a copy of anything: the SSL
	 * context and the WebSocket stream are bound to one endpoint and one
	 * executor, and the frame buffer is the storage the last @c read handed a
	 * view into.
	 */
	stream_reader(const stream_reader &)            = delete;
	stream_reader &operator=(const stream_reader &) = delete;
	/**
	 * @brief Move a reader, leaving the source owning no stream.
	 *
	 * @post The moved-from reader is *usable*, not merely destructible: every
	 *       member answers as though the stream were closed - @c is_open() is
	 *       false, @c frames() and @c connects() are zero, @c read() reports
	 *       "not connected", @c close() does nothing, and @c connect() fails
	 *       with a message saying so rather than reopening it. It owns no
	 *       endpoint to reconnect to, so that last one is the honest answer.
	 *       Spelled out because the alternative was a type whose only defined
	 *       operation after a move was destruction, which is a sharp edge on
	 *       something the session layer moves into a coroutine frame.
	 */
	TRANSPORT_EXPORT stream_reader(stream_reader &&) noexcept;
	TRANSPORT_EXPORT stream_reader &operator=(stream_reader &&) noexcept;

	/**
	 * @brief Resolve, connect, and complete both the TLS and WebSocket
	 *        handshakes.
	 *
	 * Callable again after a close or a failure, which is how a caller
	 * reconnects: the previous stream is discarded and a fresh one built. Note
	 * what reconnecting means upstream - the frames missed while disconnected
	 * are a sequence gap, so whatever is reconstructing from this feed has to
	 * be told its replica is stale.
	 * @return Nothing on success, or a human-readable error string.
	 */
	[[nodiscard]] TRANSPORT_EXPORT
		boost::asio::awaitable<std::expected<void, std::string>>
		connect();

	/**
	 * @brief The next frame.
	 *
	 * @return The frame's text, or why there was not one.
	 * @warning The view points into this reader's own buffer and is valid only
	 *          until the next @c read(). That is deliberate - a live feed at
	 * ten frames a second still has no reason to allocate a string per frame
	 * when every decoder in the tree takes a
	 *          @c std::string_view - but a caller that needs to retain a frame
	 *          must copy it.
	 * @note A failed read closes the stream, so @c is_open() is false
	 *       afterwards and the reader is ready to be reconnected.
	 */
	[[nodiscard]] TRANSPORT_EXPORT
		boost::asio::awaitable<std::expected<std::string_view, stream_status>>
		read();

	/**
	 * @brief Write one text frame.
	 *
	 * @param text The frame's payload. Borrowed for the call.
	 * @return Nothing on success, or why the write failed.
	 *
	 * @par Why a reader has a write at all
	 * Because not every WebSocket a venue offers is a firehose. A market-data
	 * stream is subscribed by its URL and never written to, which is what this
	 * class was built for; a venue's *request/response* socket - Binance's
	 * WebSocket API, where the account event stream now lives - has to be sent
	 * a subscribe request before it sends anything back. One write at the start
	 * of a conversation is the whole requirement, and a second class duplicating
	 * the connect, the TLS policy and the reconnect accounting to get it would
	 * be worse than this method.
	 *
	 * @warning Never concurrently with @c read(). A bare TCP socket tolerates a
	 *          read and a write in flight together; an SSL stream does not,
	 *          because both directions run through one non-thread-safe @c SSL
	 *          object. The supported shape is therefore a conversation - write,
	 *          then read - and not full duplex. @c rest::request_pipeline draws
	 *          the same line and its header argues it at length.
	 */
	[[nodiscard]] TRANSPORT_EXPORT
		boost::asio::awaitable<std::expected<void, std::string>>
		send(std::string_view text);

	/// @brief Close gracefully. A truncated close from the peer is not an
	///        error; nothing here is worth failing over on the way out.
	TRANSPORT_EXPORT boost::asio::awaitable<void> close();

	/// @brief Whether the stream is connected and readable.
	[[nodiscard]] TRANSPORT_EXPORT bool is_open() const noexcept;

	/// @brief Frames handed over since construction, across reconnects.
	[[nodiscard]] TRANSPORT_EXPORT std::uint64_t frames() const noexcept;

	/// @brief Successful connects since construction. Above one means the feed
	///        dropped and was rebuilt, and therefore that a gap occurred.
	[[nodiscard]] TRANSPORT_EXPORT std::uint64_t connects() const noexcept;

private:
	struct impl;
	core::util::indirect<impl> impl_;
};

/**
 * @brief Stream a text WebSocket feed to @p outfile, one frame per line.
 * @param host Endpoint host; also the SNI.
 * @param port Endpoint port.
 * @param target Stream path.
 * @param outfile Destination JSONL file (truncated).
 * @param duration How long to record before closing.
 * @param verify Certificate policy for the stream. Defaults to
 *        @c tls_verify::peer - a capture is replayed and backtested against
 *        later, so an unverified one poisons every run that reads it.
 * @return Nothing on success, or a human-readable error string.
 * @note Take the three endpoint fields from a market_data endpoint descriptor
 *       (e.g. @c binance::diff_depth_stream) rather than spelling a venue's
 *       stream name at the call site.
 */
TRANSPORT_EXPORT boost::asio::awaitable<std::expected<void, std::string>>
capture_to_file(std::string host, std::string port, std::string target,
				std::string outfile, std::chrono::seconds duration,
				tls_verify verify = tls_verify::peer);

/**
 * @brief Blocking convenience wrapper around @ref capture_to_file: spins up a
 *        local io_context and records for @p duration.
 * @return Nothing on success, or a human-readable error string.
 */
TRANSPORT_EXPORT std::expected<void, std::string>
capture(std::string host, std::string port, std::string target,
		std::string outfile, std::chrono::seconds duration,
		tls_verify verify = tls_verify::peer);

} // namespace exchange::transport::ws
