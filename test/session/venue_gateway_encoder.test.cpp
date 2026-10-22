#include "session/venue_gateway.hpp"
#include "venue/order_encoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

// A second venue is a second encoder, not an edit to the gateway.
//
// The gateway's policy - credentials, the breaker, the caps, the weight budget -
// is venue-neutral; only the wire format and the rate-limit headers are the
// venue's. This suite drives the real policy through a venue that is not
// Binance, so a Binance name leaking back into the gateway fails here.

using exchange::at_tick;
using exchange::side_t;
using exchange::engine::symbol_spec;
using exchange::engine::orders::order;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::session::basic_venue_gateway;
using exchange::session::gateway_limits;
using exchange::session::gateway_refusal;
using exchange::transport::rest::header;
using exchange::venue::credentials;
using exchange::venue::environment;
using exchange::venue::http_endpoint;
using exchange::venue::order_encoder;
using exchange::venue::outbound_cancel;
using exchange::venue::outbound_order;
using exchange::venue::outbound_replace;

namespace {

/// What the stub venue encodes to: where, and under which key.
struct encoder_stub_request {
	http_endpoint endpoint;
	std::string api_key;
};

/// A venue with its own paths, weights and header names - and nothing else.
struct encoder_stub_venue {
	static constexpr int ORDER_WEIGHT         = 3;
	static constexpr int OPEN_ORDERS_WEIGHT   = 5;
	static constexpr int TRADE_HISTORY_WEIGHT = 7;

	static constexpr std::string_view API_KEY_HEADER     = "X-STUB-KEY";
	static constexpr std::string_view USED_WEIGHT_HEADER = "X-STUB-WEIGHT";
	static constexpr std::string_view USED_ORDER_COUNT_HEADER = "X-STUB-10S";
	static constexpr std::string_view USED_ORDER_COUNT_1D_HEADER = "X-STUB-1D";

	static std::optional<encoder_stub_request>
	signed_for(std::string target, const credentials &creds) {
		return encoder_stub_request{
			.endpoint = {.host = "stub.example", .target = std::move(target)},
			.api_key  = creds.key};
	}
	static auto encode_place(const outbound_order &, const credentials &c,
							 std::int64_t, environment) {
		return signed_for("/place", c);
	}
	static auto encode_replace(const outbound_replace &, const credentials &c,
							   std::int64_t, environment) {
		return signed_for("/replace", c);
	}
	static auto encode_cancel(const outbound_cancel &, const credentials &c,
							  std::int64_t, environment) {
		return signed_for("/cancel", c);
	}
	static auto encode_open_orders(std::string_view, const credentials &c,
								   std::int64_t, environment) {
		return signed_for("/open", c);
	}
	static auto encode_trade_history(std::string_view, std::int64_t,
									 const credentials &c, std::int64_t,
									 environment, std::int64_t) {
		return signed_for("/trades", c);
	}
	static std::optional<int> parse_used(std::string_view raw) noexcept {
		int used = 0;
		const auto [end, ec] =
			std::from_chars(raw.data(), raw.data() + raw.size(), used);
		if (ec != std::errc{} || end != raw.data() + raw.size()) return std::nullopt;
		return used;
	}
};

static_assert(order_encoder<encoder_stub_venue>);

using encoder_stub_gateway = basic_venue_gateway<encoder_stub_venue>;

[[nodiscard]] symbol_spec encoder_listing() {
	return symbol_spec{7,
					   "SOLUSDT",
					   2,
					   3,
					   1 * exchange::units::scaled_price,
					   1 * exchange::units::scaled_size,
					   exchange::at_scaled(15345),
					   symbol_spec::NO_COLLAR};
}

[[nodiscard]] order encoder_order() {
	order o{};
	o.id    = 42;
	o.side  = side_t::bid;
	o.type  = order_type::LIMIT;
	o.tif   = time_in_force_instruction::GOOD_TILL_CANCELLED;
	o.price = at_tick(15345);
	o.qty   = 1500 * exchange::units::lot;
	return o;
}

[[nodiscard]] encoder_stub_gateway encoder_gateway(gateway_limits limits = {}) {
	return encoder_stub_gateway{
		credentials{.key = "stub-key", .secret = "stub-secret"},
		environment::testnet,
		limits,
		nullptr};
}

} // namespace

TEST(VenueGatewayEncoder, APlacementUsesTheVenuesPathKeyAndWeight) {
	encoder_stub_gateway gateway = encoder_gateway();
	const auto built             = gateway.place(encoder_order(),
                                     encoder_listing(),
                                     "SOLUSDT",
                                     1,
                                     encoder_stub_gateway::clock::time_point{});
	ASSERT_TRUE(built.has_value());
	EXPECT_EQ(built->host, "stub.example");
	EXPECT_EQ(built->request.target, "/place");
	ASSERT_EQ(built->request.headers.size(), 1U);
	EXPECT_EQ(built->request.headers[0].name, "X-STUB-KEY");
	EXPECT_EQ(built->request.headers[0].value, "stub-key");
	EXPECT_EQ(built->weight, encoder_stub_venue::ORDER_WEIGHT);
}

TEST(VenueGatewayEncoder, TheVenuesWeightHeaderIsTheOneAdopted) {
	encoder_stub_gateway gateway = encoder_gateway();
	const auto now               = encoder_stub_gateway::clock::time_point{};
	const int before             = gateway.spare_weight(now);
	const std::array<header, 2> headers{
		header{.name = "X-MBX-USED-WEIGHT-1M", .value = "500"},
		header{.name = "X-STUB-WEIGHT", .value = "999"}};
	gateway.observe(headers, now);
	EXPECT_EQ(gateway.spare_weight(now), before - 999)
		<< "the stub venue's header is the one adopted, not Binance's";
}

TEST(VenueGatewayEncoder, ThePolicyIsTheGatewaysWhateverTheVenue) {
	encoder_stub_gateway gateway =
		encoder_gateway(gateway_limits{.max_orders = 1});
	const auto now = encoder_stub_gateway::clock::time_point{};
	ASSERT_TRUE(
		gateway.place(encoder_order(), encoder_listing(), "SOLUSDT", 1, now));
	const auto second =
		gateway.place(encoder_order(), encoder_listing(), "SOLUSDT", 1, now);
	ASSERT_FALSE(second.has_value());
	EXPECT_EQ(second.error(), gateway_refusal::order_cap_reached);
}
