#pragma once
// What a REST request is, independent of what moves it.
//
// `rest` used to take (host, target) and always mean GET: enough for market
// data, which only ever reads. Anything that *writes* to a venue needs a verb,
// a body and at least one header it chose itself - an API key is a header - so
// the request became a value, and the one-shot and pipelined senders both take
// it.
//
// Deliberately Boost-free, like the rest of this module's public surface.

#include "transport/rest/method.hpp" // IWYU pragma: export

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exchange::transport::rest {

/// @brief One request header. Name is matched case-insensitively by servers;
///        it is sent as spelled.
struct header {
	std::string name{};
	std::string value{};

	bool operator==(const header &) const noexcept = default;
};

/**
 * @brief The value of the first header named @p name, or nothing.
 *
 * @note Case-insensitive, because HTTP field names are (RFC 9110 §5.1) and
 *       servers do not agree on capitalisation - Binance sends
 *       @c x-mbx-used-weight-1m on some paths and @c X-MBX-USED-WEIGHT-1M on
 *       others. Matching exactly is how a header goes quietly missing.
 * @note A linear scan: a response carries a dozen headers and a caller reads
 *       one or two of them, so an index would cost more to build than it saves.
 */
[[nodiscard]] inline std::optional<std::string_view>
find_header(std::span<const header> headers, std::string_view name) noexcept {
	const auto same = [](char a, char b) noexcept {
		return std::tolower(static_cast<unsigned char>(a)) ==
			   std::tolower(static_cast<unsigned char>(b));
	};
	for (const header &h : headers)
		if (std::ranges::equal(h.name, name, same)) return h.value;
	return std::nullopt;
}

/**
 * @brief One HTTP request: everything except which connection carries it.
 *
 * @note @c Host, @c User-Agent, @c Accept, @c Content-Type and @c
 * Content-Length are set by the sender - the first from the connection's host,
 * the last two from @c body and @c content_type. Naming any of them in @c
 * headers would send it twice, so don't.
 */
struct request {
	/// @brief The verb. @see is_idempotent, which is read from it on the
	///        pipeline's retry path.
	method verb = method::get;

	/// @brief Request path including query, e.g.
	///        @c /api/v3/order?symbol=SOLUSDT&side=BUY&signature=...
	std::string target{};

	/// @brief Headers this request chose - an API key, an idempotency token.
	///        Empty for public reads.
	std::vector<header> headers{};

	/// @brief Request body, empty for a request that has none. A GET with a
	///        body is legal and universally mishandled; don't.
	std::string body{};

	/**
	 * @brief @c Content-Type for @c body, sent only when @c body is non-empty.
	 *
	 * Defaulted to form encoding because that is what venue order APIs take -
	 * Binance accepts its parameters either in the query string or as
	 * @c application/x-www-form-urlencoded, and never as JSON.
	 */
	std::string content_type = "application/x-www-form-urlencoded";
};

/// @brief A GET of @p target with no headers and no body - the shape every
///        public market-data read has.
[[nodiscard]] inline request get_request(std::string target) {
	return request{.verb = method::get, .target = std::move(target)};
}

/**
 * @brief Whether an unanswered @p req may be put back on the wire.
 *
 * Both conditions have to hold: the caller must have asked for the degrade
 * behaviour at all, and the method must be one where a duplicate is harmless.
 * A POST that was written and never answered is in the worst state there is -
 * the venue may or may not have acted on it - and the only correct response is
 * to go and ask, which is the caller's job and not this module's.
 *
 * @param req The request that went out unanswered.
 * @param degrade Whether the caller allows re-sending at all.
 * @see pipeline_options::degrade_on_failure
 */
[[nodiscard]] constexpr bool may_resend(const request &req,
										bool degrade) noexcept {
	return degrade && is_idempotent(req.verb);
}

} // namespace exchange::transport::rest
