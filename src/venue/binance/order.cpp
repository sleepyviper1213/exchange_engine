#include "venue/binance/order.hpp"

#include "core/scaled/decimal.hpp"
#include "venue/binance/host.hpp"
#include "venue/binance/signing.hpp"

#include <fmt/format.h>

#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::venue::binance {
namespace {

using engine::orders::order_type;
using engine::orders::time_in_force_instruction;

/// The venue's spelling of a side. Binance says BUY/SELL where the book says
/// bid/ask - the same distinction, named from the taker's point of view.
[[nodiscard]] constexpr std::string_view venue_side(side_t side) noexcept {
	return side == side_t::bid ? "BUY" : "SELL";
}

/// The venue's spelling of an order type, for the two that are sendable.
[[nodiscard]] constexpr std::string_view venue_type(order_type type) noexcept {
	switch (type) {
	case order_type::MARKET: return "MARKET";
	case order_type::LIMIT: return "LIMIT";
	case order_type::STOP: return {};
	}
	return {};
}

/// The venue's spelling of a duration, for the three that map.
///
/// ALL_OR_NONE deliberately has no answer: Binance has no such time in force,
/// and the nearest thing - FOK - is a different instruction (it also demands
/// immediacy). Encoding one as the other would send an order the caller did not
/// ask for, so it is refused instead.
[[nodiscard]] constexpr std::string_view
venue_tif(time_in_force_instruction tif) noexcept {
	switch (tif) {
	case time_in_force_instruction::GOOD_TILL_CANCELLED: return "GTC";
	case time_in_force_instruction::FILL_OR_KILL: return "FOK";
	case time_in_force_instruction::IMMEDIATE_OR_CANCEL: return "IOC";
	case time_in_force_instruction::ALL_OR_NONE: return {};
	}
	return {};
}

/// Everything a signed request needs once its own parameters are written.
[[nodiscard]] std::string with_auth_tail(std::string query,
										 std::int64_t timestamp_ms) {
	return fmt::format("{}&recvWindow={}&timestamp={}",
					   query,
					   RECV_WINDOW_MS,
					   timestamp_ms);
}

/// Assemble the finished request from a query that is complete but unsigned.
[[nodiscard]] signed_request finish(std::string_view path, std::string query,
									const credentials &creds, int weight,
									environment env) {
	const std::string signed_query = sign_query(query, creds.secret);
	return signed_request{
		.endpoint =
			http_endpoint{.host   = std::string(host_for(env).rest),
						  .target = fmt::format("{}?{}", path, signed_query)},
		.api_key = creds.key,
		.weight  = weight};
}

/// The checks every signed request shares.
[[nodiscard]] std::optional<encode_error>
common_refusal(std::string_view symbol, const credentials &creds) {
	if (!creds.is_complete()) return encode_error::no_credentials;
	if (symbol.empty()) return encode_error::no_symbol;
	return std::nullopt;
}

} // namespace

std::string_view describe(encode_error why) noexcept {
	switch (why) {
	case encode_error::no_credentials:
		return "no API key and secret configured";
	case encode_error::no_symbol: return "the order names no listing";
	case encode_error::no_client_id: return "the order carries no client id";
	case encode_error::bad_quantity: return "quantity is not positive";
	case encode_error::bad_price: return "a limit order needs a positive price";
	case encode_error::unsupported_type:
		return "only LIMIT and MARKET can be sent";
	case encode_error::unsupported_tif:
		return "only GTC, IOC and FOK have a venue equivalent";
	}
	return "unknown encoding failure";
}

namespace {

/// @brief @p order's own parameters, from @c symbol through @c quantity, with
///        @p mode spliced in after @c type where the venue documents it. Shared
///        by a placement and a replacement so the two cannot spell one order
///        two ways.
[[nodiscard]] std::expected<std::string, encode_error>
order_query(const outbound_order &order, std::string_view mode = {}) {
	if (order.client_order_id.empty())
		return std::unexpected(encode_error::no_client_id);
	if (order.qty_scaled <= 0)
		return std::unexpected(encode_error::bad_quantity);

	const std::string_view type = venue_type(order.type);
	if (type.empty()) return std::unexpected(encode_error::unsupported_type);

	// Fixed order, and the reason is not style: the signature covers these
	// bytes exactly as they go out, so reordering them here without reordering
	// them in the signature is a request the venue rejects as unauthorised.
	std::string query = fmt::format("symbol={}&side={}&type={}",
									order.symbol,
									venue_side(order.side),
									type);
	if (!mode.empty()) query += fmt::format("&cancelReplaceMode={}", mode);

	if (order.has_price()) {
		if (order.price_scaled <= 0)
			return std::unexpected(encode_error::bad_price);
		const std::string_view tif = venue_tif(order.tif);
		if (tif.empty()) return std::unexpected(encode_error::unsupported_tif);
		// timeInForce is only meaningful on a LIMIT: Binance refuses it on a
		// MARKET order rather than ignoring it.
		query += fmt::format(
			"&timeInForce={}&price={}",
			tif,
			core::scaled::to_decimal(order.price_scaled, order.price_scale));
	}

	query += fmt::format(
		"&quantity={}",
		core::scaled::to_decimal(order.qty_scaled, order.qty_scale));
	return query;
}

} // namespace

std::expected<signed_request, encode_error>
place_order(const outbound_order &order, const credentials &creds,
			std::int64_t timestamp_ms, environment env) {
	if (const auto refused = common_refusal(order.symbol, creds))
		return std::unexpected(*refused);
	auto query = order_query(order);
	if (!query) return std::unexpected(query.error());

	*query += fmt::format("&newClientOrderId={}", order.client_order_id);
	return finish("/api/v3/order",
				  with_auth_tail(std::move(*query), timestamp_ms),
				  creds,
				  ORDER_WEIGHT,
				  env);
}

std::expected<signed_request, encode_error>
cancel_replace_order(const outbound_replace &replace, const credentials &creds,
					 std::int64_t timestamp_ms, environment env) {
	const outbound_order &order = replace.replacement;
	if (const auto refused = common_refusal(order.symbol, creds))
		return std::unexpected(*refused);
	if (replace.cancel_client_order_id.empty())
		return std::unexpected(encode_error::no_client_id);
	auto query = order_query(order, "STOP_ON_FAILURE");
	if (!query) return std::unexpected(query.error());

	*query += fmt::format("&cancelOrigClientOrderId={}&newClientOrderId={}",
						  replace.cancel_client_order_id,
						  order.client_order_id);
	return finish("/api/v3/order/cancelReplace",
				  with_auth_tail(std::move(*query), timestamp_ms),
				  creds,
				  ORDER_WEIGHT,
				  env);
}

std::optional<std::vector<std::string>>
parse_open_order_ids(std::string_view json) try {
	simdjson::dom::parser parser;
	simdjson::dom::element doc;
	if (parser.parse(simdjson::padded_string(json)).get(doc) !=
		simdjson::SUCCESS)
		return std::nullopt;
	simdjson::dom::array orders;
	if (doc.get_array().get(orders) != simdjson::SUCCESS) return std::nullopt;

	std::vector<std::string> ids;
	ids.reserve(orders.size());
	for (const simdjson::dom::element entry : orders) {
		std::string_view id;
		// One entry without the field is skipped rather than failing the
		// whole read: the rest of the list is still the venue's word.
		if (entry["clientOrderId"].get_string().get(id) != simdjson::SUCCESS)
			continue;
		ids.emplace_back(id);
	}
	return ids;
} catch (...) { return std::nullopt; }

std::expected<signed_request, encode_error>
account_info(const credentials &creds, std::int64_t timestamp_ms,
			 environment env) {
	if (!creds.is_complete())
		return std::unexpected(encode_error::no_credentials);
	return finish("/api/v3/account",
				  with_auth_tail("omitZeroBalances=true", timestamp_ms),
				  creds,
				  ACCOUNT_WEIGHT,
				  env);
}

std::optional<std::vector<asset_balance>>
parse_free_balances(std::string_view json) try {
	simdjson::dom::parser parser;
	simdjson::dom::element doc;
	if (parser.parse(simdjson::padded_string(json)).get(doc) !=
		simdjson::SUCCESS)
		return std::nullopt;
	simdjson::dom::array balances;
	if (doc["balances"].get_array().get(balances) != simdjson::SUCCESS)
		return std::nullopt;

	std::vector<asset_balance> out;
	out.reserve(balances.size());
	for (const simdjson::dom::element entry : balances) {
		std::string_view asset;
		std::string_view free;
		if (entry["asset"].get_string().get(asset) != simdjson::SUCCESS ||
			entry["free"].get_string().get(free) != simdjson::SUCCESS)
			continue;
		out.push_back({.asset = std::string(asset), .free = std::string(free)});
	}
	return out;
} catch (...) { return std::nullopt; }

std::optional<std::string> parse_ticker_price(std::string_view json) try {
	simdjson::dom::parser parser;
	simdjson::dom::element doc;
	if (parser.parse(simdjson::padded_string(json)).get(doc) !=
		simdjson::SUCCESS)
		return std::nullopt;
	std::string_view price;
	if (doc["price"].get_string().get(price) != simdjson::SUCCESS)
		return std::nullopt;
	return std::string(price);
} catch (...) { return std::nullopt; }

replace_failure classify_replace_failure(std::string_view body) noexcept try {
	simdjson::dom::parser parser;
	simdjson::dom::element doc;
	if (parser.parse(simdjson::padded_string(body)).get(doc) !=
		simdjson::SUCCESS)
		return replace_failure::unchanged;

	std::int64_t code = 0;
	if (doc["code"].get_int64().get(code) != simdjson::SUCCESS)
		return replace_failure::unchanged;
	if (code == REPLACE_PARTIALLY_FAILED) return replace_failure::withdrawn;
	if (code != REPLACE_FAILED) return replace_failure::unchanged;

	std::int64_t cancel_code = 0;
	if (doc.at_pointer("/data/cancelResponse/code")
			.get_int64()
			.get(cancel_code) != simdjson::SUCCESS)
		return replace_failure::unchanged;
	return cancel_code == UNKNOWN_ORDER ? replace_failure::order_gone
										: replace_failure::unchanged;
} catch (...) {
	// The DOM parser allocates; exhaustion is the only throw, and the safe
	// reading of a body we could not decode is that nothing moved.
	return replace_failure::unchanged;
}

std::expected<signed_request, encode_error>
cancel_order(const outbound_cancel &cancel, const credentials &creds,
			 std::int64_t timestamp_ms, environment env) {
	if (const auto refused = common_refusal(cancel.symbol, creds))
		return std::unexpected(*refused);
	if (cancel.client_order_id.empty())
		return std::unexpected(encode_error::no_client_id);

	// origClientOrderId, not orderId: cancelling by our own identifier works
	// before the placement's ack has come back with the venue's.
	std::string query = fmt::format("symbol={}&origClientOrderId={}",
									cancel.symbol,
									cancel.client_order_id);

	return finish("/api/v3/order",
				  with_auth_tail(std::move(query), timestamp_ms),
				  creds,
				  ORDER_WEIGHT,
				  env);
}

std::expected<signed_request, encode_error>
open_orders(std::string_view symbol, const credentials &creds,
			std::int64_t timestamp_ms, environment env) {
	if (const auto refused = common_refusal(symbol, creds))
		return std::unexpected(*refused);

	// Scoped to one listing on purpose: the unscoped form costs 80 weight
	// against a 6000-per-minute budget and returns orders for listings this
	// process does not trade.
	return finish(
		"/api/v3/openOrders",
		with_auth_tail(fmt::format("symbol={}", symbol), timestamp_ms),
		creds,
		OPEN_ORDERS_WEIGHT,
		env);
}

} // namespace exchange::venue::binance
