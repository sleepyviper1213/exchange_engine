#pragma once
// What comes back, and why it sometimes does not.
//
// Split from the senders on purpose: `pipeline.hpp` needs `failure` and needs
// nothing about the one-shot client, and a caller that only inspects a result
// should not compile Boost.Asio to do it.

#include "transport/rest/request.hpp" // IWYU pragma: export - header, find_header
#include "transport_export.hpp"       // TRANSPORT_EXPORT (generated)

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <vector>

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
 *       its own business and is decoded where that venue is known - for
 *       Binance, @c venue::binance::parse_api_error.
 */
struct failure {
	/// @brief HTTP status, or zero when there was never a response at all -
	///        DNS, connect, TLS or timeout. Zero is therefore "we do not know
	///        whether the venue would have refused us", which is a different
	///        thing from any 4xx and must not be treated as one.
	unsigned status = 0;

	/// @brief The response body, when there was one. Kept whole rather than
	///        summarised: it is where a venue explains itself.
	std::string body{};

	/**
	 * @brief What @c Retry-After asked for, when the server sent it.
	 *
	 * Only ever populated on 429 and 418, which are the only two responses
	 * Binance documents it on. Absent means "the server did not say", not "wait
	 * zero" - a caller with no guidance should use its own backoff rather than
	 * retry immediately.
	 */
	std::optional<std::chrono::seconds> retry_after{};

	/// @brief The transport-level reason, when @c status is zero.
	std::string detail{};

	/// @brief The response headers, when there was a response.
	///
	/// Carried on the failure arm as well as the success one because the
	/// responses that carry the most operationally useful headers are the ones
	/// that failed: a 429 states the venue's running rate-limit count in the
	/// same header a 200 does, and that is precisely the moment a caller needs
	/// to read it. @see find_header
	std::vector<header> headers{};

	/// @brief Whether the venue refused this for rate-limiting - 429, or the
	///        418 that follows ignoring one.
	[[nodiscard]] bool is_rate_limited() const noexcept {
		return status == STATUS_TOO_MANY_REQUESTS || status == STATUS_IP_BANNED;
	}

	/// @brief Whether we have been banned rather than merely throttled.
	///
	/// Binance returns 418 when a client keeps sending after a 429, and the ban
	/// is on the *address*, escalating from two minutes to three days on
	/// repetition. A client that sees this and carries on is making it worse,
	/// so it is worth being able to ask.
	[[nodiscard]] bool is_ip_banned() const noexcept {
		return status == STATUS_IP_BANNED;
	}

	/// @brief Whether retrying the identical request could ever succeed.
	///
	/// False for 4xx other than the two rate-limit codes: a malformed request
	/// or an unknown symbol will be refused identically for ever, and retrying
	/// it spends rate-limit budget to learn nothing. @see risk_gate, which
	/// draws the same distinction between back-pressure and a refusal.
	[[nodiscard]] bool is_retryable() const noexcept {
		if (status == 0)
			return true; // never reached the venue; the link may heal
		if (is_rate_limited()) return true;
		return status >= STATUS_SERVER_ERROR;
	}

	/// @brief One line, for a log.
	/// @note The only member defined out of line - it is the one that needs
	///       fmt, and this header is included by callers that do not.
	[[nodiscard]] TRANSPORT_EXPORT std::string message() const;
};

/**
 * @brief What a request that succeeded came back with.
 *
 * @note The headers are handed back whole rather than picked over here.
 *       @c Retry-After is the one exception, and only because RFC 9110 defines
 *       it - everything else a venue puts in a header is that venue's business
 *       and is read where the venue is known. @c X-MBX-USED-WEIGHT-1M is the
 *       motivating case: transport must not learn Binance's spelling of its
 *       own rate limit. @see venue::binance
 */
struct reply {
	/// @brief The response body.
	std::string body{};

	/// @brief Every header the response carried, in the order sent.
	std::vector<header> headers{};
};

/**
 * @brief One response, or why it could not be had.
 *
 * The single result type of this module: what @c https_request answers with,
 * and what a pipelined batch holds one of per request. Naming it once is what
 * lets a caller that pipelines and a caller that does not handle results with
 * the same code.
 */
using response = std::expected<reply, failure>;

} // namespace exchange::transport::rest
