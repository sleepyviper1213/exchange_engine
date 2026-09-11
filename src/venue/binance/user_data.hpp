#pragma once
// The return leg: Binance's user data stream, and the report it carries.
//
// Order entry is REST, but its *answers* are not. The HTTP response to a
// placement says only that the venue took the order; everything after that -
// the acknowledgement, every partial fill, the cancellation - arrives on a
// separate WebSocket stream keyed by a `listenKey`. A gateway that reads only
// HTTP responses learns nothing about its own fills.
//
// --- the listen key, and why it is a lifecycle rather than a value ----------
//
// The key is obtained with a POST, expires 60 minutes after it was issued, and
// is extended by a PUT that must arrive before then. Miss the keepalive and the
// stream closes with no error on our side - the socket simply ends, and the
// process carries on believing it is watching an account it can no longer see.
// That is why `KEEPALIVE_INTERVAL` is stated here rather than left to a caller
// to pick, and why the reconciliation read exists at all: any gap in this
// stream, from a missed keepalive or a dropped socket, means the venue's view
// and ours may have diverged and only `open_orders` can settle it.
//
// The listen key above is *withdrawn*. `POST /api/v3/userDataStream` now
// answers 410 Gone, from nginx rather than from the application, so there is
// nothing left to authenticate against. Its builders are kept because they
// still describe a real deployment somewhere, and because deleting them would
// lose the one thing an operator who hits that 410 needs: a header saying what
// replaced it.
//
// --- what replaced it, and why it is a protocol change rather than a URL ----
//
// The events now arrive on the *WebSocket API*, a request/response socket. A
// caller connects, sends a subscribe request, reads its answer, and only then
// starts reading events. Three consequences, each of which this module has to
// model:
//
//   * no key sits in a URL any more, so there is nothing to keep alive on a
//     timer - the subscription lives exactly as long as the socket does;
//   * a frame on that socket may be a *response* or an *event*, and telling
//     them apart is the reader's job (@see is_user_data_event);
//   * an event is wrapped - {"subscriptionId":N,"event":{...}} - so the
//     executionReport a parser wants is one level down (@see unwrap_event).
//
// Signed with HMAC rather than logged in with Ed25519, deliberately.
// `userDataStream.subscribe.signature` takes the same apiKey/timestamp/
// signature triple every other request in this tree already builds, where
// `session.logon` needs an Ed25519 key pair registered with the venue - a
// second credential to issue, store and rotate, for no capability this needs.
//
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/user-data-stream
// @see
// https://developers.binance.com/docs/binance-spot-api-docs/websocket-api/user-data-stream-requests

#include "core/util/enum_string.hpp"
#include "venue/credentials.hpp"
#include "venue/endpoint.hpp"
#include "venue/environment.hpp"
#include "venue/execution_report.hpp"
#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace exchange::venue::binance {

/// @brief Rate-limit weight of any of the three listen-key requests.
inline constexpr int LISTEN_KEY_WEIGHT = 2;

/**
 * @brief How long a listen key survives without a keepalive.
 *
 * The venue's number, not a choice. Stated so the interval below can be derived
 * from it rather than from memory.
 */
inline constexpr std::chrono::minutes LISTEN_KEY_LIFETIME{60};

/**
 * @brief How often to send the keepalive PUT.
 *
 * Half the lifetime, so a single missed or failed keepalive still leaves a full
 * interval to retry in before the key dies. Binance's own examples suggest 30
 * minutes and this is that number, arrived at deliberately: a keepalive is 2
 * weight against 6000 per minute, so sending it more often costs nothing worth
 * counting and losing the stream costs the account's visibility.
 */
inline constexpr std::chrono::minutes KEEPALIVE_INTERVAL{30};

#define BINANCE_USER_DATA_ERROR_LIST(X)                                        \
	X(no_credentials, "no API key configured")                                 \
	X(no_listen_key, "no listen key was given")                                \
	X(invalid_json, "invalid JSON")                                            \
	X(missing_field, "missing or mistyped field")                              \
	X(not_an_execution_report, "the frame is not an executionReport")          \
	X(bad_number, "a price or quantity was not a valid decimal")               \
	X(subscribe_refused, "the venue declined the user data subscription")

/// @brief Why a user-data request could not be built or a frame decoded.
enum class user_data_error : std::uint8_t {
	EXCHANGE_ENUM_VALUES(BINANCE_USER_DATA_ERROR_LIST)
};

/// @brief The category message for a @c user_data_error.
EXCHANGE_ENUM_LABEL(user_data_error, message, BINANCE_USER_DATA_ERROR_LIST)

#undef BINANCE_USER_DATA_ERROR_LIST

/**
 * @brief A request that carries the API key but is not signed.
 *
 * The three listen-key endpoints are Binance's @c USER_STREAM security type:
 * they need the key header to say *whose* stream, and no signature, because
 * they neither move money nor read a balance. Signing one anyway is not
 * harmless - the venue rejects a request carrying a signature it did not ask
 * for.
 */
struct keyed_request {
	http_endpoint endpoint{};

	/// @brief The @c X-MBX-APIKEY header value.
	std::string api_key{};

	/// @brief Rate-limit weight to debit before sending. @see weight_budget
	int weight = LISTEN_KEY_WEIGHT;
};

/// @brief @c POST @c /api/v3/userDataStream - obtain a listen key.
[[nodiscard]] VENUE_EXPORT std::expected<keyed_request, user_data_error>
open_listen_key(const credentials &creds,
				environment env = environment::testnet);

/**
 * @brief @c PUT @c /api/v3/userDataStream - extend @p listen_key's life.
 * @note Must be sent at least every @c LISTEN_KEY_LIFETIME; @see
 *       KEEPALIVE_INTERVAL for the interval to actually use.
 */
[[nodiscard]] VENUE_EXPORT std::expected<keyed_request, user_data_error>
keepalive_listen_key(std::string_view listen_key, const credentials &creds,
					 environment env = environment::testnet);

/// @brief @c DELETE @c /api/v3/userDataStream - close the stream early.
/// @note Worth sending on a clean shutdown: an abandoned key holds a stream
///       slot until it expires, and an account has a limited number of them.
[[nodiscard]] VENUE_EXPORT std::expected<keyed_request, user_data_error>
close_listen_key(std::string_view listen_key, const credentials &creds,
				 environment env = environment::testnet);

/// @brief Read the @c listenKey field out of a @c POST response body.
[[nodiscard]] VENUE_EXPORT std::expected<std::string, user_data_error>
parse_listen_key(std::string_view json);

/**
 * @brief The WebSocket stream carrying @p listen_key's account events.
 *
 * @warning The key is in the URL path. It is a bearer credential for the
 *          account's event stream - do not log this endpoint, and note that the
 *          @c stream_endpoint formatter prints the target verbatim.
 */
[[nodiscard]] VENUE_EXPORT stream_endpoint user_data_stream(
	std::string_view listen_key, environment env = environment::testnet);

/**
 * @brief The WebSocket API socket for @p env - where account events now arrive.
 *
 * @note Carries no credential, unlike the endpoint a listen key produced.
 *       Authentication happens in a *message* on this socket rather than in its
 *       URL, which is the one unambiguous improvement in the change: this
 *       endpoint is safe to log.
 */
[[nodiscard]] VENUE_EXPORT stream_endpoint
ws_api_endpoint(environment env = environment::testnet);

/**
 * @brief The signed @c userDataStream.subscribe.signature request to write once
 *        the socket is open.
 *
 * @param creds Key and secret. Both, unlike the listen key: that was a
 *        @c USER_STREAM endpoint taking the key alone, and this is signed.
 * @param timestamp_ms Wall-clock milliseconds, for the venue's replay window.
 * @param request_id Echoed in the response's @c id, which is how a reader knows
 *        the answer it has is the answer to this.
 * @return The JSON text frame to send, or why it could not be built.
 *
 * @warning The result carries the API key and a signature over the request. It
 *          is a credential - do not log it.
 */
[[nodiscard]] VENUE_EXPORT std::expected<std::string, user_data_error>
subscribe_request(const credentials &creds, std::int64_t timestamp_ms,
				  std::string_view request_id);

/**
 * @brief Whether @p json is a pushed event rather than an answer to a request.
 *
 * The discriminator is the *absence* of @c status: a response carries one and
 * an event does not. Read this way round rather than by looking for @c event,
 * because then an unrecognised push is still classified as a push - which keeps
 * an event type nobody has modelled yet from being read as a malformed
 * response.
 */
[[nodiscard]] VENUE_EXPORT bool is_user_data_event(std::string_view json);

/**
 * @brief Confirm a subscribe response, or say why the venue refused.
 *
 * @param json The frame answering @c subscribe_request.
 * @return The subscription id, or @c subscribe_refused for any status but 200.
 *         The venue's own words are not carried out: a caller that wants them
 *         still has the frame, and copying them into an enum would lose them.
 */
[[nodiscard]] VENUE_EXPORT std::expected<std::int64_t, user_data_error>
parse_subscribe_reply(std::string_view json);

/**
 * @brief The @c event object inside a pushed frame, as text.
 *
 * @return The nested object, or @p json unchanged when there is no @c event
 *         wrapper - which is what the old listen-key stream sent, and what a
 *         test hands @c parse_execution_report directly. Permissive on purpose:
 *         one parser for both shapes cannot disagree with itself, where two
 *         would.
 * @note Returns a view *into* @p json, so it does not outlive it.
 */
[[nodiscard]] VENUE_EXPORT std::string_view
unwrap_event(std::string_view json) noexcept;

/**
 * @brief Decode one @c executionReport frame.
 *
 * @param json One frame from the user data stream.
 * @param price_decimals Scale to read prices on - the listing's, from
 *        @c symbol_filters::price_decimals.
 * @param qty_decimals Scale to read quantities on. Frequently not equal to
 *        @p price_decimals.
 * @return The report, or why the frame could not be used.
 *
 * @note A frame of another event type - @c outboundAccountPosition,
 *       @c balanceUpdate, @c listStatus - is @c not_an_execution_report rather
 *       than a parse failure. The stream is multiplexed and a caller that wants
 *       only fills must be able to skip the rest without treating each one as
 *       an error.
 * @note An unrecognised status or execution type decodes to @c unknown /
 *       @c other rather than failing: the venue may add one at any time, and
 *       dropping the whole frame would discard the fill it also carried.
 */
[[nodiscard]] VENUE_EXPORT std::expected<execution_report, user_data_error>
parse_execution_report(std::string_view json, int price_decimals,
					   int qty_decimals);

} // namespace exchange::venue::binance
