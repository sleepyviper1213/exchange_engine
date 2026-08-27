#pragma once
// The venue's own explanation of a refusal.
//
// Binance answers every rejected REST request with the same two-field envelope,
// whatever the HTTP status: {"code": -1121, "msg": "Invalid symbol."}. The status
// says which *class* of thing went wrong; only the body says what. Reporting the
// status alone leaves an operator holding "HTTP 400" and a JSON blob to squint
// at, which is a diagnosis they have to make rather than one we made.
//
// Deliberately takes a string rather than a transport type. `market_data/` does
// not depend on `transport/` - endpoints.hpp hands out a resolved
// {host, port, target} and something else moves the bytes - and decoding an error
// body is no reason to add that edge. The join happens at the call site, which
// already names both.

#include "market_data_export.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace exchange::market_data::binance {

/**
 * @brief A Binance REST error body.
 *
 * @note @c code is negative in every documented case, and grouped: -1000s are
 *       general or malformed-request errors, -1100s are parameter problems,
 *       -2010 and below are order rejections. It is kept as the integer the
 *       venue sent rather than mapped onto a local enum, because the set is
 *       long, versioned by them, and only ever displayed here - inventing a
 *       local name for each would be a table to maintain for no reader's
 *       benefit.
 */
struct api_error {
	int code = 0;     ///< the venue's error code, e.g. -1121
	std::string msg;  ///< the venue's message, e.g. "Invalid symbol."

	bool operator==(const api_error &) const noexcept = default;
};

/**
 * @brief Decode a Binance error envelope from @p body.
 *
 * @return The envelope, or nothing if @p body is not one - which is the ordinary
 *         case for a body that is empty, is HTML from an edge proxy, or is a
 *         successful payload somebody passed here by mistake. A caller should
 *         fall back to the HTTP status it already has rather than treat the
 *         absence as an error of its own.
 *
 * @note Never throws and allocates only the message. A failure path that can
 *       itself fail loudly is worse than the failure it is reporting.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::optional<api_error>
parse_api_error(std::string_view body) noexcept;

/**
 * @brief @p body's message and code as one line, or @p fallback if it is not an
 *        error envelope.
 *
 * The shape a log line wants, so callers do not each re-write the same
 * "did it parse, and if so how do I say it" branch.
 */
[[nodiscard]] MARKET_DATA_EXPORT std::string
describe_api_error(std::string_view body, std::string_view fallback);

} // namespace exchange::market_data::binance
