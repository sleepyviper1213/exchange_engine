#pragma once

#include "transport_export.h" // TRANSPORT_EXPORT (generated)

#include <boost/asio/awaitable.hpp>

#include <expected>
#include <string>

namespace exchange::transport::rest {

/**
 * @brief One-shot HTTPS GET returning the response body.
 * @param host TLS host (e.g. @c api.binance.com); also the SNI + Host header.
 * @param target Request path with query (e.g. @c /api/v3/depth?symbol=SOLUSDT).
 * @return The response body on HTTP 200, or a human-readable error string.
 */
TRANSPORT_EXPORT boost::asio::awaitable<std::expected<std::string, std::string>>
https_get(std::string host, std::string target);

/**
 * @brief Blocking convenience wrapper around @ref https_get: spins up a local
 *        io_context, runs one GET to completion, and returns the body.
 * @param host TLS host.
 * @param target Request path with query.
 * @return The response body, or a human-readable error string.
 */
TRANSPORT_EXPORT std::expected<std::string, std::string> get(std::string host,
															 std::string target);

} // namespace exchange::transport::rest
