#pragma once
// Binance spot as a venue::order_encoder: the five signed requests the
// gateway sends, their weights, and the headers that report what was spent.
// Nothing here decides anything - it is the wire format, named so the gateway
// can be written without naming Binance.

#include "venue/binance/order.hpp"
#include "venue/binance/rate_limit.hpp"
#include "venue/order_encoder.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace exchange::venue::binance {

struct encoder {
	static constexpr int ORDER_WEIGHT         = binance::ORDER_WEIGHT;
	static constexpr int OPEN_ORDERS_WEIGHT   = binance::OPEN_ORDERS_WEIGHT;
	static constexpr int TRADE_HISTORY_WEIGHT = MY_TRADES_WEIGHT;

	static constexpr std::string_view API_KEY_HEADER = "X-MBX-APIKEY";
	static constexpr std::string_view USED_WEIGHT_HEADER =
		binance::USED_WEIGHT_HEADER;
	static constexpr std::string_view USED_ORDER_COUNT_HEADER =
		binance::USED_ORDER_COUNT_HEADER;
	static constexpr std::string_view USED_ORDER_COUNT_1D_HEADER =
		binance::USED_ORDER_COUNT_1D_HEADER;

	[[nodiscard]] static auto encode_place(const outbound_order &order,
										   const credentials &creds,
										   std::int64_t timestamp_ms,
										   environment env) {
		return place_order(order, creds, timestamp_ms, env);
	}

	[[nodiscard]] static auto encode_replace(const outbound_replace &replace,
											 const credentials &creds,
											 std::int64_t timestamp_ms,
											 environment env) {
		return cancel_replace_order(replace, creds, timestamp_ms, env);
	}

	[[nodiscard]] static auto encode_cancel(const outbound_cancel &cancel,
											const credentials &creds,
											std::int64_t timestamp_ms,
											environment env) {
		return cancel_order(cancel, creds, timestamp_ms, env);
	}

	[[nodiscard]] static auto encode_open_orders(std::string_view symbol,
												 const credentials &creds,
												 std::int64_t timestamp_ms,
												 environment env) {
		return open_orders(symbol, creds, timestamp_ms, env);
	}

	[[nodiscard]] static auto
	encode_trade_history(std::string_view symbol, std::int64_t from_id,
						 const credentials &creds, std::int64_t timestamp_ms,
						 environment env, std::int64_t start_ms) {
		return my_trades(symbol, from_id, creds, timestamp_ms, env, start_ms);
	}

	[[nodiscard]] static std::optional<int>
	parse_used(std::string_view header) noexcept {
		return parse_used_weight(header);
	}
};

static_assert(order_encoder<encoder>);

} // namespace exchange::venue::binance
