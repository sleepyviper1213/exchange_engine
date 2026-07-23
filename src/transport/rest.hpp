#pragma once

#include "transport_export.h" // TRANSPORT_EXPORT (generated)

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>

#include <expected>
#include <string>

namespace transport::rest {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ssl   = asio::ssl;
using tcp       = asio::ip::tcp;

// as_tuple delivers each completion as a tuple led by the error_code, so
// failures stay values (no exceptions thrown across co_await) and each step
// unpacks its own result with a structured binding.
inline constexpr auto token = asio::as_tuple(asio::use_awaitable);

/**
 * @brief One-shot HTTPS GET returning the response body.
 * @param host TLS host (e.g. @c api.binance.com); also the SNI + Host header.
 * @param target Request path with query (e.g. @c /api/v3/depth?symbol=SOLUSDT).
 * @return The response body on HTTP 200, or a human-readable error string.
 */
TRANSPORT_EXPORT asio::awaitable<std::expected<std::string, std::string>>
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

} // namespace transport::rest
