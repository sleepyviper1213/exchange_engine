#include "venue/binance/order.hpp"

#include "core/scaled/decimal.hpp"
#include "venue/binance/host.hpp"
#include "venue/binance/signing.hpp"

#include <fmt/format.h>

#include <optional>
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

std::expected<signed_request, encode_error>
place_order(const outbound_order &order, const credentials &creds,
			std::int64_t timestamp_ms, environment env) {
	if (const auto refused = common_refusal(order.symbol, creds))
		return std::unexpected(*refused);
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

	query +=
		fmt::format("&quantity={}&newClientOrderId={}",
					core::scaled::to_decimal(order.qty_scaled, order.qty_scale),
					order.client_order_id);

	return finish("/api/v3/order",
				  with_auth_tail(std::move(query), timestamp_ms),
				  creds,
				  ORDER_WEIGHT,
				  env);
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
