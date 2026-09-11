#pragma once
// Binance's spelling of an order, and of withdrawing one.
//
// The Adapter proper: our vocabulary in, the venue's query string out, signed.
// Nothing here opens a socket - it produces the {endpoint, request} pair that
// `session/` hands to transport, which is the same contract every other
// endpoint builder in the tree follows.
//
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/rest-api/trading-endpoints

#include "venue/binance/api_error.hpp"
#include "venue/credentials.hpp"
#include "venue/endpoint.hpp"
#include "venue/environment.hpp"
#include "venue/outbound_order.hpp"
#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace exchange::venue::binance {

/// @brief Rate-limit weight of one order placement or cancellation.
inline constexpr int ORDER_WEIGHT = 1;

/// @brief Rate-limit weight of one open-orders query for a single symbol.
inline constexpr int OPEN_ORDERS_WEIGHT = 6;

/**
 * @brief How long after @c timestamp the venue will still accept a request.
 *
 * Binance refuses anything whose @c timestamp is more than @c recvWindow
 * milliseconds old, which is the replay defence: a signed request captured off
 * the wire stops being usable almost immediately. 5000 is the venue's own
 * default and its documented maximum is 60000; larger is not safer.
 */
inline constexpr std::int64_t RECV_WINDOW_MS = 5000;

/// @brief A request that is ready to send: where it goes, and what it says.
///
/// @note @c signed_request::request carries the API key in a header and the
///       signature in the target's query string. Do not log either whole - the
///       @c http_endpoint formatter prints the target verbatim and is for
///       unsigned endpoints only.
struct signed_request {
	http_endpoint endpoint{};

	/// @brief The @c X-MBX-APIKEY header value. Held separately from the
	///        endpoint so a caller assembles the transport request itself and
	///        this module keeps no transport edge.
	std::string api_key{};

	/// @brief Rate-limit weight to debit before sending. @see weight_budget
	int weight = 0;
};

/**
 * @brief Why an order could not be turned into a request.
 *
 * Encoding failures, not venue refusals - a venue refusal arrives as an
 * @c api_error in a response body. These are the ones caught before anything
 * is sent, which is where they are cheapest.
 */
enum class encode_error : std::uint8_t {
	no_credentials,   ///< key or secret missing @see credentials::is_complete
	no_symbol,        ///< the listing was not named
	no_client_id,     ///< nothing to reconcile the order back to
	bad_quantity,     ///< non-positive size
	bad_price,        ///< a limit order with a non-positive price
	unsupported_type, ///< STOP - nothing here watches a trigger
	unsupported_tif,  ///< ALL_OR_NONE has no venue equivalent
};

/// @brief One line naming @p why, for a log or a reject reason.
[[nodiscard]] VENUE_EXPORT std::string_view describe(encode_error why) noexcept;

/**
 * @brief Encode @p order as a signed @c POST @c /api/v3/order.
 *
 * @param order What to send. Prices and sizes are already scaled.
 * @param creds The API credential; both halves must be present.
 * @param timestamp_ms Wall-clock milliseconds since the Unix epoch. Passed in
 *        rather than read here so the encoding is a pure function and its
 *        signature is reproducible in a test.
 * @param env Which deployment. @see host_for
 * @return The request, or why it could not be built.
 *
 * @note The parameters are appended in a fixed order and the signature is
 *       computed over the finished query, because what is signed and what is
 *       sent have to be identical byte for byte. Do not reorder them.
 */
[[nodiscard]] VENUE_EXPORT std::expected<signed_request, encode_error>
place_order(const outbound_order &order, const credentials &creds,
			std::int64_t timestamp_ms, environment env = environment::testnet);

/**
 * @brief Encode @p cancel as a signed @c DELETE @c /api/v3/order.
 * @see place_order for the parameters and the ordering rule.
 */
[[nodiscard]] VENUE_EXPORT std::expected<signed_request, encode_error>
cancel_order(const outbound_cancel &cancel, const credentials &creds,
			 std::int64_t timestamp_ms, environment env = environment::testnet);

/**
 * @brief Encode a signed @c GET @c /api/v3/openOrders for one listing.
 *
 * The reconciliation read: what the venue believes is working, which is the
 * only way to find out what an unanswered placement actually did. @see
 * may_resend, which is why that question has to be askable.
 */
[[nodiscard]] VENUE_EXPORT std::expected<signed_request, encode_error>
open_orders(std::string_view symbol, const credentials &creds,
			std::int64_t timestamp_ms, environment env = environment::testnet);

} // namespace exchange::venue::binance
