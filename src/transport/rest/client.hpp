#pragma once
// One request, one connection: resolve, connect, handshake, write, read, close.
//
// The right shape for a request that stands alone - a depth snapshot, an order,
// a reference-data read at startup. For many requests to one host,
// `pipeline.hpp` keeps the connection and pays the handshake once.

#include "options.hpp"          // IWYU pragma: export
#include "request.hpp"          // IWYU pragma: export
#include "response.hpp"         // IWYU pragma: export
#include "transport_export.hpp" // TRANSPORT_EXPORT (generated)

#include <boost/asio/awaitable.hpp>

#include <string>

namespace exchange::transport::rest {

/**
 * @brief One-shot HTTPS request over a connection opened and closed for it.
 *
 * @param host TLS host (e.g. @c api.binance.com); also the SNI + Host header.
 * @param req What to send. @see request
 * @param options Certificate policy and deadline. Verification is @b on by
 *        default here - @see tls_verify for why this differs from
 *        @ref https_get.
 * @return The response body on HTTP 200, or why not. @see failure
 *
 * @note A non-200 status is a @c failure, not an exception, and the body comes
 *       back inside it: a venue explains a refusal in the body it refuses with.
 * @note Whether a request that was written and never answered may be sent again
 *       is not decided here - this function either completes or reports why it
 *       could not, once. @see may_resend for the rule a retrying caller owes.
 */
TRANSPORT_EXPORT boost::asio::awaitable<response>
https_request(std::string host, request req, request_options options = {});

/**
 * @brief Blocking wrapper around @ref https_request: spins up a local
 *        io_context and runs one request to completion.
 * @param host TLS host.
 * @param req What to send.
 * @param options Certificate policy and deadline.
 * @return The response body, or why not. @see failure
 */
TRANSPORT_EXPORT response send(std::string host, request req,
							   request_options options = {});

/**
 * @brief One-shot HTTPS GET returning the response body.
 *
 * @param host TLS host (e.g. @c api.binance.com); also the SNI + Host header.
 * @param target Request path with query (e.g. @c /api/v3/depth?symbol=SOLUSDT).
 * @return The response body on HTTP 200, or why not. @see failure
 *
 * @note Certificate verification is @b off, which is why this is a separate
 *       function rather than a default argument on @ref https_request. It is
 *       the public-market-data reader, where TLS is wanted for integrity and
 *       the tree must still run on a machine with no CA bundle installed.
 *       Never send a credential through it. @see tls_verify
 */
TRANSPORT_EXPORT boost::asio::awaitable<response> https_get(std::string host,
															std::string target);

/**
 * @brief Blocking convenience wrapper around @ref https_get: spins up a local
 *        io_context, runs one GET to completion, and returns the body.
 * @param host TLS host.
 * @param target Request path with query.
 * @return The response body, or why not. @see failure
 * @note Unverified, exactly as @ref https_get. @see tls_verify
 */
TRANSPORT_EXPORT response get(std::string host, std::string target);

} // namespace exchange::transport::rest
