#pragma once

#include "transport_export.hpp" // TRANSPORT_EXPORT (generated)

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>

namespace exchange::transport::rest {

/// @brief The response arrived and carried a body.
inline constexpr unsigned STATUS_OK = 200;

/// @brief Rate limit exceeded. Carries @c Retry-After.
inline constexpr unsigned STATUS_TOO_MANY_REQUESTS = 429;

/// @brief The address is banned for having ignored a 429. Also carries
///        @c Retry-After, and the ban escalates on repetition.
inline constexpr unsigned STATUS_IP_BANNED = 418;

/// @brief First status that is the *server's* problem, and therefore worth
///        retrying where a 4xx is not.
inline constexpr unsigned STATUS_SERVER_ERROR = 500;

/**
 * @brief Why a request did not return a body.
 *
 * @par Why this is a type and not a string
 * Because one caller has to *act* on the answer rather than log it. A request
 * refused for rate-limiting must be retried later and a request refused for a
 * bad symbol must not be retried at all, and the two are indistinguishable once
 * they have been formatted into a sentence. The retry decision then ends up
 * being made by whoever happens to own the loop, on no information - which is
 * how a client walks into an IP ban.
 *
 * @note Protocol-level and venue-agnostic on purpose. The status codes and
 *       @c Retry-After are HTTP's; what a particular venue puts in the body is
 *       its own business and is decoded where that venue is known - for Binance,
 *       @c market_data::binance::parse_api_error.
 */
struct failure {
	/// @brief HTTP status, or zero when there was never a response at all -
	///        DNS, connect, TLS or timeout. Zero is therefore "we do not know
	///        whether the venue would have refused us", which is a different
	///        thing from any 4xx and must not be treated as one.
	unsigned status = 0;

	/// @brief The response body, when there was one. Kept whole rather than
	///        summarised: it is where a venue explains itself.
	std::string body;

	/**
	 * @brief What @c Retry-After asked for, when the server sent it.
	 *
	 * Only ever populated on 429 and 418, which are the only two responses
	 * Binance documents it on. Absent means "the server did not say", not "wait
	 * zero" - a caller with no guidance should use its own backoff rather than
	 * retry immediately.
	 */
	std::optional<std::chrono::seconds> retry_after;

	/// @brief The transport-level reason, when @c status is zero.
	std::string detail;

	/// @brief Whether the venue refused this for rate-limiting - 429, or the
	///        418 that follows ignoring one.
	[[nodiscard]] bool is_rate_limited() const noexcept {
		return status == STATUS_TOO_MANY_REQUESTS || status == STATUS_IP_BANNED;
	}

	/// @brief Whether we have been banned rather than merely throttled.
	///
	/// Binance returns 418 when a client keeps sending after a 429, and the ban
	/// is on the *address*, escalating from two minutes to three days on
	/// repetition. A client that sees this and carries on is making it worse, so
	/// it is worth being able to ask.
	[[nodiscard]] bool is_ip_banned() const noexcept {
		return status == STATUS_IP_BANNED;
	}

	/// @brief Whether retrying the identical request could ever succeed.
	///
	/// False for 4xx other than the two rate-limit codes: a malformed request or
	/// an unknown symbol will be refused identically for ever, and retrying it
	/// spends rate-limit budget to learn nothing. @see risk_gate, which draws
	/// the same distinction between back-pressure and a refusal.
	[[nodiscard]] bool is_retryable() const noexcept {
		if (status == 0) return true; // never reached the venue; the link may heal
		if (is_rate_limited()) return true;
		return status >= STATUS_SERVER_ERROR;
	}

	/// @brief One line, for a log.
	[[nodiscard]] TRANSPORT_EXPORT std::string message() const;
};

/**
 * @brief One-shot HTTPS GET returning the response body.
 * @param host TLS host (e.g. @c api.binance.com); also the SNI + Host header.
 * @param target Request path with query (e.g. @c /api/v3/depth?symbol=SOLUSDT).
 * @return The response body on HTTP 200, or why not. @see failure
 */
TRANSPORT_EXPORT boost::asio::awaitable<std::expected<std::string, failure>>
https_get(std::string host, std::string target);

/**
 * @brief Blocking convenience wrapper around @ref https_get: spins up a local
 *        io_context, runs one GET to completion, and returns the body.
 * @param host TLS host.
 * @param target Request path with query.
 * @return The response body, or why not. @see failure
 */
TRANSPORT_EXPORT std::expected<std::string, failure>
get(std::string host, std::string target);

} // namespace exchange::transport::rest
