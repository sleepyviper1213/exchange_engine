#pragma once

// WebSocket transport: stream a text WebSocket feed to a JSONL file, one frame
// per line - the capture path that feeds offline replay (see
// transport/replay.hpp and benchmark/market_replay.cpp).
//
// Deliberately venue-agnostic. This layer opens a socket, upgrades it, and
// writes frames; it does not know that the frames are Binance `depthUpdate`
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
#include <expected>
#include <string>

namespace exchange::transport::ws {

/**
 * @brief Stream a text WebSocket feed to @p outfile, one frame per line.
 * @param host Endpoint host; also the SNI.
 * @param port Endpoint port.
 * @param target Stream path.
 * @param outfile Destination JSONL file (truncated).
 * @note Take the three endpoint fields from a market-data endpoint descriptor
 *       (e.g. @c binance::diff_depth_stream) rather than spelling a venue's
 *       stream name at the call site.
 * @param duration How long to record before closing.
 * @return Nothing on success, or a human-readable error string.
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
