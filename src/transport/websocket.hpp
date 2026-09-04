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

#include "transport_export.hpp" // TRANSPORT_EXPORT (generated)

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
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
	 * @param host Endpoint host; also the SNI.
	 * @param port Endpoint port.
	 * @param target Stream path.
	 * @note Take the three fields from a market_data endpoint descriptor (e.g.
	 *       @c binance::diff_depth_stream) rather than spelling a venue's
	 *       stream name at the call site.
	 */
	TRANSPORT_EXPORT stream_reader(std::string host, std::string port,
								   std::string target);
	TRANSPORT_EXPORT ~stream_reader();
	stream_reader(const stream_reader &)            = delete;
	stream_reader &operator=(const stream_reader &) = delete;
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
	std::unique_ptr<impl> impl_;
};

/**
 * @brief Stream a text WebSocket feed to @p outfile, one frame per line.
 * @param host Endpoint host; also the SNI.
 * @param port Endpoint port.
 * @param target Stream path.
 * @param outfile Destination JSONL file (truncated).
 * @param duration How long to record before closing.
 * @return Nothing on success, or a human-readable error string.
 * @note Take the three endpoint fields from a market_data endpoint descriptor
 *       (e.g. @c binance::diff_depth_stream) rather than spelling a venue's
 *       stream name at the call site.
 */
TRANSPORT_EXPORT boost::asio::awaitable<std::expected<void, std::string>>
capture_to_file(std::string host, std::string port, std::string target,
				std::string outfile, std::chrono::seconds duration);

/**
 * @brief Blocking convenience wrapper around @ref capture_to_file: spins up a
 *        local io_context and records for @p duration.
 * @return Nothing on success, or a human-readable error string.
 */
TRANSPORT_EXPORT std::expected<void, std::string>
capture(std::string host, std::string port, std::string target,
		std::string outfile, std::chrono::seconds duration);

} // namespace exchange::transport::ws
