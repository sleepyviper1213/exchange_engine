#include "venue/binance/user_data.hpp"

#include "core/scaled/fixed_point.hpp"
#include "venue/binance/host.hpp"
#include "venue/binance/signing.hpp"

#include <fmt/format.h>

#include <simdjson.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::venue::binance {
namespace {

/**
 * Which of our two decode failures a simdjson error is.
 *
 * On-Demand parses lazily: `iterate()` accepts a buffer it has not looked at,
 * so a body that is not JSON at all - an HTML error page from an edge proxy is
 * the common one - survives until the first field is read and fails *there*.
 * Only these two codes mean "the JSON was well formed and the field was not in
 * it"; everything else means the document was never JSON, and saying "missing
 * or mistyped field" about a 502 page sends an operator looking for a field.
 */
[[nodiscard]] user_data_error json_error(simdjson::error_code err) noexcept {
	return err == simdjson::NO_SUCH_FIELD || err == simdjson::INCORRECT_TYPE
			   ? user_data_error::missing_field
			   : user_data_error::invalid_json;
}

/// Whether @p doc's top level is a JSON object at all.
///
/// Checked before any field is read, because `type()` peeks the first token
/// without consuming it - and a document whose top level is a string or an HTML
/// page is malformed input rather than an object missing a field.
[[nodiscard]] bool is_object(simdjson::ondemand::document &doc) noexcept {
	simdjson::ondemand::json_type type{};
	if (doc.type().get(type)) return false;
	return type == simdjson::ondemand::json_type::object;
}

/// The venue's status strings onto ours. Unrecognised is a value, not a
/// failure - @see execution_status::unknown.
[[nodiscard]] execution_status status_from(std::string_view text) noexcept {
	if (text == "NEW") return execution_status::accepted;
	if (text == "PARTIALLY_FILLED") return execution_status::partially_filled;
	if (text == "FILLED") return execution_status::filled;
	if (text == "CANCELED") return execution_status::cancelled;
	if (text == "PENDING_CANCEL") return execution_status::pending_cancel;
	if (text == "REJECTED") return execution_status::rejected;
	// EXPIRED_IN_MATCH is a self-trade-prevention expiry; it is still an
	// expiry as far as anything downstream is concerned.
	if (text == "EXPIRED" || text == "EXPIRED_IN_MATCH")
		return execution_status::expired;
	return execution_status::unknown;
}

/// The venue's execution types onto ours. @see execution_kind
[[nodiscard]] execution_kind kind_from(std::string_view text) noexcept {
	if (text == "NEW") return execution_kind::acknowledgement;
	if (text == "TRADE") return execution_kind::trade;
	if (text == "CANCELED") return execution_kind::cancellation;
	if (text == "REJECTED") return execution_kind::rejection;
	if (text == "EXPIRED" || text == "TRADE_PREVENTION")
		return execution_kind::expiry;
	return execution_kind::other;
}

/// A string field, or empty when absent. Absence is ordinary here: half these
/// fields only appear on some report kinds.
[[nodiscard]] std::string read_string(simdjson::ondemand::document &doc,
									  std::string_view key) {
	std::string_view value;
	if (doc[key].get_string().get(value)) return {};
	return std::string(value);
}

/// A signed integer field, or @p fallback when absent or mistyped.
[[nodiscard]] std::int64_t read_i64(simdjson::ondemand::document &doc,
									std::string_view key,
									std::int64_t fallback = 0) {
	std::int64_t value = fallback;
	if (doc[key].get_int64().get(value)) return fallback;
	return value;
}

/// A boolean field, or @p fallback when absent or mistyped.
[[nodiscard]] bool read_bool(simdjson::ondemand::document &doc,
							 std::string_view key, bool fallback = false) {
	bool value = fallback;
	if (doc[key].get_bool().get(value)) return fallback;
	return value;
}

/// A decimal-string field scaled to an integer, or 0 when the field is absent.
///
/// Absent is 0 rather than an error because the venue omits or zeroes most of
/// these on the reports they do not apply to. A field that is *present and
/// malformed* is a genuine failure and says so - that is a wire change, not an
/// optional field.
[[nodiscard]] std::expected<std::int64_t, user_data_error>
read_scaled(simdjson::ondemand::document &doc, std::string_view key,
			int scale) {
	std::string_view text;
	if (doc[key].get_string().get(text)) return 0;
	const auto parsed = core::scaled::parse_fixed_point(text, scale);
	if (!parsed) return std::unexpected(user_data_error::bad_number);
	return *parsed;
}

/// The three listen-key requests differ only in verb and query, and the verb is
/// the caller's to apply - this module hands out an endpoint, not a request.
[[nodiscard]] std::expected<keyed_request, user_data_error>
listen_key_request(std::string query, const credentials &creds,
				   environment env) {
	// The key alone, deliberately: USER_STREAM endpoints take no signature, and
	// `creds.secret` is neither read nor needed here.
	if (creds.key.empty())
		return std::unexpected(user_data_error::no_credentials);

	std::string target = "/api/v3/userDataStream";
	if (!query.empty()) target += "?" + query;
	return keyed_request{
		.endpoint = http_endpoint{.host   = std::string(host_for(env).rest),
								  .target = std::move(target)},
		.api_key  = creds.key,
		.weight   = LISTEN_KEY_WEIGHT};
}

} // namespace

std::expected<keyed_request, user_data_error>
open_listen_key(const credentials &creds, environment env) {
	return listen_key_request({}, creds, env);
}

std::expected<keyed_request, user_data_error>
keepalive_listen_key(std::string_view listen_key, const credentials &creds,
					 environment env) {
	if (listen_key.empty())
		return std::unexpected(user_data_error::no_listen_key);
	return listen_key_request(fmt::format("listenKey={}", listen_key),
							  creds,
							  env);
}

std::expected<keyed_request, user_data_error>
close_listen_key(std::string_view listen_key, const credentials &creds,
				 environment env) {
	if (listen_key.empty())
		return std::unexpected(user_data_error::no_listen_key);
	return listen_key_request(fmt::format("listenKey={}", listen_key),
							  creds,
							  env);
}

std::expected<std::string, user_data_error>
parse_listen_key(std::string_view json) try {
	simdjson::ondemand::parser parser;
	simdjson::padded_string padded{json};
	simdjson::ondemand::document doc;
	if (parser.iterate(padded).get(doc))
		return std::unexpected(user_data_error::invalid_json);

	if (!is_object(doc)) return std::unexpected(user_data_error::invalid_json);

	std::string_view key;
	if (const auto err = doc["listenKey"].get_string().get(key))
		return std::unexpected(json_error(err));
	if (key.empty()) return std::unexpected(user_data_error::missing_field);
	return std::string(key);
} catch (...) {
	// simdjson allocates, so the only thing that throws here is exhaustion. A
	// body we could not decode is reported as malformed, which is what the
	// caller's failure path is already for.
	return std::unexpected(user_data_error::invalid_json);
}

stream_endpoint user_data_stream(std::string_view listen_key, environment env) {
	const hosts at = host_for(env);
	return stream_endpoint{.host   = std::string(at.stream),
						   .port   = std::string(at.stream_port),
						   .target = fmt::format("/ws/{}", listen_key)};
}

std::expected<execution_report, user_data_error>
parse_execution_report(std::string_view json, int price_decimals,
					   int qty_decimals) try {
	simdjson::ondemand::parser parser;
	simdjson::padded_string padded{json};
	simdjson::ondemand::document doc;
	if (parser.iterate(padded).get(doc))
		return std::unexpected(user_data_error::invalid_json);

	if (!is_object(doc)) return std::unexpected(user_data_error::invalid_json);

	// The event type first, because the stream is multiplexed: an account
	// position update is a perfectly good frame that is simply not this.
	std::string_view event;
	if (const auto err = doc["e"].get_string().get(event))
		return std::unexpected(json_error(err));
	if (event != "executionReport")
		return std::unexpected(user_data_error::not_an_execution_report);

	execution_report report;
	report.symbol          = read_string(doc, "s");
	report.client_order_id = read_string(doc, "c");
	// `C` carries the *cancelled* order's id on a cancel report, and Binance
	// sends it as an empty string rather than omitting it on every other kind.
	report.original_client_order_id = read_string(doc, "C");
	report.venue_order_id           = read_i64(doc, "i");
	report.status                   = status_from(read_string(doc, "X"));
	report.kind                     = kind_from(read_string(doc, "x"));
	report.event_time_ms            = read_i64(doc, "E");
	report.transaction_time_ms      = read_i64(doc, "T", report.event_time_ms);
	report.is_maker                 = read_bool(doc, "m");
	report.reject_reason            = read_string(doc, "r");

	// "NONE" is what the venue sends when nothing was rejected, which is not a
	// reason and should not read as one downstream.
	if (report.reject_reason == "NONE") report.reject_reason.clear();

	struct field {
		std::string_view key;
		int scale;
		std::int64_t *into;
	};

	const field numbers[] = {
		{"l", qty_decimals, &report.last_qty_scaled},
		{"L", price_decimals, &report.last_price_scaled},
		{"z", qty_decimals, &report.cumulative_qty_scaled},
		{"q", qty_decimals, &report.order_qty_scaled},
		{"p", price_decimals, &report.order_price_scaled},
	};
	for (const field &f : numbers) {
		const auto value = read_scaled(doc, f.key, f.scale);
		if (!value) return std::unexpected(value.error());
		*f.into = *value;
	}

	return report;
} catch (...) { return std::unexpected(user_data_error::invalid_json); }

// --- the WebSocket API, which is where the account stream lives now ---------

stream_endpoint ws_api_endpoint(environment env) {
	// Port 443 rather than the 9443 the market-data streams use. Two hosts,
	// two conventions, and guessing either from the other is the mistake
	// `host_for` exists to stop anyone making.
	return stream_endpoint{.host   = std::string(host_for(env).ws_api),
						   .port   = "443",
						   .target = "/ws-api/v3"};
}

std::expected<std::string, user_data_error>
subscribe_request(const credentials &creds, std::int64_t timestamp_ms,
				  std::string_view request_id) {
	if (!creds.is_complete())
		return std::unexpected(user_data_error::no_credentials);

	// Signed exactly as a REST request is: the parameters in the venue's
	// canonical order, joined as a query string, HMACed whole. It is the same
	// `sign_query` every other signed request here uses, which is the point of
	// choosing the signature method over an Ed25519 session logon.
	const std::string payload = fmt::format("apiKey={}&timestamp={}",
											creds.key,
											timestamp_ms);
	const std::string signature = sign(payload, creds.secret);

	return fmt::format(R"({{"id":"{}","method":"userDataStream.subscribe.)"
					   R"(signature","params":{{"apiKey":"{}","timestamp":{},)"
					   R"("signature":"{}"}}}})",
					   request_id,
					   creds.key,
					   timestamp_ms,
					   signature);
}

bool is_user_data_event(std::string_view json) try {
	simdjson::ondemand::parser parser;
	simdjson::padded_string padded{json};
	simdjson::ondemand::document doc;
	if (parser.iterate(padded).get(doc)) return false;
	if (!is_object(doc)) return false;

	// The absence of `status` is the discriminator. @see the header on why this
	// way round rather than looking for `event`.
	std::int64_t status = 0;
	return doc["status"].get_int64().get(status) != simdjson::SUCCESS;
} catch (...) {
	// A frame we could not even scan is not an event. It reaches the response
	// path, which reports it as malformed - which is the honest answer, where
	// classifying it as an event would have it silently discarded.
	return false;
}

std::expected<std::int64_t, user_data_error>
parse_subscribe_reply(std::string_view json) try {
	simdjson::ondemand::parser parser;
	simdjson::padded_string padded{json};
	simdjson::ondemand::document doc;
	if (parser.iterate(padded).get(doc))
		return std::unexpected(user_data_error::invalid_json);
	if (!is_object(doc)) return std::unexpected(user_data_error::invalid_json);

	std::int64_t status = 0;
	if (const auto err = doc["status"].get_int64().get(status))
		return std::unexpected(json_error(err));
	// Any status but 200 is a refusal, and the venue's explanation is in the
	// frame the caller still holds. @see the header.
	if (status != 200)
		return std::unexpected(user_data_error::subscribe_refused);

	// A subscription id of zero is normal - it is an index, and the first one
	// is the first - so its absence has to be read from the error rather than
	// from the value.
	std::int64_t id = 0;
	if (doc["result"]["subscriptionId"].get_int64().get(id))
		return std::int64_t{0};
	return id;
} catch (...) {
	return std::unexpected(user_data_error::invalid_json);
}

std::string_view unwrap_event(std::string_view json) noexcept {
	// Deliberately textual rather than a parse-and-reserialise. simdjson's
	// On-Demand cursor hands back a view of the raw bytes for a nested object,
	// so the inner document is already in `json` and copying it out would cost
	// an allocation per frame on the return path. A frame that does not carry
	// the wrapper is handed back whole, which is what makes one parser serve
	// both shapes. @see the header.
	static constexpr std::string_view TAG = R"("event":)";
	const std::size_t at                  = json.find(TAG);
	if (at == std::string_view::npos) return json;

	std::size_t open = json.find('{', at + TAG.size());
	if (open == std::string_view::npos) return json;

	// Brace matching rather than a search for the last `}`: an execution report
	// nests, and a string inside it may contain a brace of its own - a client
	// order id is caller-chosen text. So strings are skipped whole, and an
	// escape inside one is stepped over rather than being allowed to end it.
	int depth        = 0;
	bool in_string   = false;
	bool escaped     = false;
	for (std::size_t i = open; i < json.size(); ++i) {
		const char ch = json[i];
		if (in_string) {
			if (escaped) escaped = false;
			else if (ch == '\\') escaped = true;
			else if (ch == '"') in_string = false;
			continue;
		}
		if (ch == '"') in_string = true;
		else if (ch == '{') ++depth;
		else if (ch == '}' && --depth == 0)
			return json.substr(open, i - open + 1);
	}
	// Unbalanced, so there is no object to hand back. The whole frame goes to
	// the parser, which reports it as malformed rather than this pretending to
	// have found something.
	return json;
}

} // namespace exchange::venue::binance
