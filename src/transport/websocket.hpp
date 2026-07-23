#pragma once

// WebSocket transport: stream a text WebSocket feed to a JSONL file, one frame
// per line — the capture path that feeds offline replay (see
// transport/replay.hpp and benchmark/market_replay.cpp). Protocol-agnostic: the
// caller supplies the host/port/target (e.g. Binance stream.binance.com:9443
// /ws/solusdt@depth@100ms). Header-only; link OpenSSL + Boost (see
// transport/CMakeLists).
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

#include "transport_export.h" // TRANSPORT_EXPORT (generated)

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <expected>
#include <string>

namespace transport::ws {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace ssl       = asio::ssl;
using tcp           = asio::ip::tcp;

// as_tuple turns each completion into a tuple led by the error_code, so
// failures stay values (no exceptions across co_await) — the same token
// rest::https_get uses.
inline constexpr auto token = asio::as_tuple(asio::use_awaitable);

/**
 * @brief Stream a text WebSocket feed to @p outfile, one frame per line.
 * @param host Endpoint host (e.g. @c stream.binance.com); also the SNI.
 * @param port Endpoint port (e.g. @c 9443).
 * @param target Stream path (e.g. @c /ws/solusdt@depth@100ms).
 * @param outfile Destination JSONL file (truncated).
 * @param duration How long to record before closing.
 * @return Nothing on success, or a human-readable error string.
 */
TRANSPORT_EXPORT asio::awaitable<std::expected<void, std::string>>
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

} // namespace transport::ws
