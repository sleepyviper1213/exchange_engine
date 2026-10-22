#pragma once
// What a venue's order-entry wire format has to supply, so the policy in front
// of it - credentials, the breaker, the order caps, the weight budget - is
// written once rather than once per venue.
//
// A concept, not a base class: the gateway is a template over its encoder, so
// the encoding is a direct call and a second venue is a second model of this
// rather than an edit to the gateway. Everything it takes and returns is
// already venue-neutral - outbound_order, outbound_cancel, outbound_replace,
// credentials, environment - except the encoded request, which only has to
// name where it goes and what key signs it.

#include "venue/credentials.hpp"
#include "venue/environment.hpp"
#include "venue/outbound_cancel.hpp"
#include "venue/outbound_order.hpp"
#include "venue/outbound_replace.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>

namespace exchange::venue {

/**
 * @brief A venue's order-entry encoding: five signed requests, what each
 *        costs against the venue's rate limit, and the headers that report
 *        what has been spent.
 *
 * Each @c E::encode_* returns something contextually convertible to @c bool
 * (false: the request cannot be stated) whose value has @c endpoint.host,
 * @c endpoint.target and @c api_key.
 */
template <class E>
concept order_encoder =
	requires(const outbound_order &order, const outbound_cancel &cancel,
			 const outbound_replace &replace, const credentials &creds,
			 std::int64_t ts, environment env, std::string_view symbol,
			 std::string_view header) {
		{ E::encode_place(order, creds, ts, env)->endpoint.target };
		{ E::encode_replace(replace, creds, ts, env)->endpoint.target };
		{ E::encode_cancel(cancel, creds, ts, env)->endpoint.target };
		{ E::encode_open_orders(symbol, creds, ts, env)->api_key };
		{ E::encode_trade_history(symbol, ts, creds, ts, env, ts)->api_key };
		{ E::ORDER_WEIGHT } -> std::convertible_to<int>;
		{ E::OPEN_ORDERS_WEIGHT } -> std::convertible_to<int>;
		{ E::TRADE_HISTORY_WEIGHT } -> std::convertible_to<int>;
		{ E::API_KEY_HEADER } -> std::convertible_to<std::string_view>;
		{ E::USED_WEIGHT_HEADER } -> std::convertible_to<std::string_view>;
		{ E::USED_ORDER_COUNT_HEADER } -> std::convertible_to<std::string_view>;
		{ E::USED_ORDER_COUNT_1D_HEADER } -> std::convertible_to<std::string_view>;
		{ E::parse_used(header) } -> std::same_as<std::optional<int>>;
	};

} // namespace exchange::venue
