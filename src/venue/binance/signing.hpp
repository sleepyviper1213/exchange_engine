#pragma once
// HMAC-SHA256 over a query string, hex-encoded - Binance's SIGNED endpoint
// authentication.
//
// The scheme is documented rather than clever: take the request's query string
// exactly as it will be sent, HMAC it with the API secret, and append the
// result as one more `signature=` parameter. What makes it easy to get wrong is
// the word *exactly* - the bytes signed and the bytes sent must be identical,
// including parameter order, so the signature is computed from the finished
// query rather than from the values that went into it.
//
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/rest-api/endpoint-security-type

#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <string>
#include <string_view>

namespace exchange::venue::binance {

/// @brief Characters in a hex-encoded SHA-256 digest.
inline constexpr std::size_t SIGNATURE_CHARS = 64;

/**
 * @brief @p payload signed with @p secret, as lowercase hex.
 *
 * @param payload The exact bytes to sign - for Binance, the query string with
 *        no leading @c ?.
 * @param secret The API secret. Not logged, not copied beyond this call.
 * @return 64 lowercase hex characters, or an empty string if the platform's
 *         HMAC refused - which it does not do for valid inputs, and which is
 *         reported as empty rather than thrown because a signing failure must
 *         not unwind through a coroutine on the I/O path.
 *
 * @note Deterministic: the same payload and secret always give the same
 *       signature, which is what makes it testable against the vector Binance
 *       publishes.
 */
[[nodiscard]] VENUE_EXPORT std::string sign(std::string_view payload,
											std::string_view secret);

/**
 * @brief @p query with its @c signature parameter appended.
 *
 * The whole of the signing convention in one function, so no caller has to
 * remember that the signature covers everything before it and goes last.
 *
 * @param query The query string, no leading @c ?, already in the order it will
 *        be sent.
 * @param secret The API secret.
 * @return @c "<query>&signature=<hex>", or @p query unchanged when @p secret is
 *         empty - an unsigned request the venue will refuse, which is a clearer
 *         failure than a request signed with nothing.
 */
[[nodiscard]] VENUE_EXPORT std::string sign_query(std::string_view query,
												  std::string_view secret);

} // namespace exchange::venue::binance
