#include "venue/binance/order.hpp"
#include "venue/binance/signing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Encoding an order into the query string Binance authenticates.
//
// The assertions worth reading are the refusals: everything caught here is
// caught before a request leaves the process, where it costs nothing. The
// alternative is finding out from a fill.

using exchange::side_t;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::venue::credentials;
using exchange::venue::environment;
using exchange::venue::is_production;
using exchange::venue::outbound_cancel;
using exchange::venue::outbound_order;
using exchange::venue::binance::cancel_order;
using exchange::venue::binance::describe;
using exchange::venue::binance::encode_error;
using exchange::venue::binance::open_orders;
using exchange::venue::binance::ORDER_WEIGHT;
using exchange::venue::binance::place_order;
using exchange::venue::binance::sign;

namespace {

/// A fixed timestamp, so an encoding is reproducible.
constexpr std::int64_t ORDER_TEST_TIME_MS = 1'499'827'319'559;

[[nodiscard]] credentials order_test_credentials() {
	return credentials{.key = "test-key", .secret = "test-secret"};
}

/// A well-formed SOLUSDT buy: 1.500 at 153.45, on the listing's real grid -
/// which has a different scale for price than for size, as most do.
[[nodiscard]] outbound_order order_test_limit_buy() {
	return outbound_order{.symbol          = "SOLUSDT",
						  .client_order_id = "eng-42",
						  .side            = side_t::bid,
						  .type            = order_type::LIMIT,
						  .tif = time_in_force_instruction::GOOD_TILL_CANCELLED,
						  .price_scaled = 15345,
						  .price_scale  = 2,
						  .qty_scaled   = 1500,
						  .qty_scale    = 3};
}

/// The target's query string, without the leading path.
[[nodiscard]] std::string order_test_query(const std::string &target) {
	const std::size_t at = target.find('?');
	return at == std::string::npos ? std::string{} : target.substr(at + 1);
}

} // namespace

TEST(BinancePlaceOrder, WritesEveryParameterTheVenueNeeds) {
	const auto request = place_order(order_test_limit_buy(),
									 order_test_credentials(),
									 ORDER_TEST_TIME_MS,
									 environment::testnet);
	ASSERT_TRUE(request.has_value()) << "a well-formed order was refused";

	const std::string query = order_test_query(request->endpoint.target);
	EXPECT_TRUE(query.contains("symbol=SOLUSDT"));
	EXPECT_TRUE(query.contains("side=BUY"));
	EXPECT_TRUE(query.contains("type=LIMIT"));
	EXPECT_TRUE(query.contains("timeInForce=GTC"));
	// Rendered at the listing's own scales, which differ from each other.
	EXPECT_TRUE(query.contains("price=153.45")) << query;
	EXPECT_TRUE(query.contains("quantity=1.500")) << query;
	EXPECT_TRUE(query.contains("newClientOrderId=eng-42"));
	EXPECT_TRUE(query.contains("timestamp=1499827319559"));
}

TEST(BinancePlaceOrder, TheSignatureCoversTheWholeQueryAndComesLast) {
	const credentials creds = order_test_credentials();
	const auto request =
		place_order(order_test_limit_buy(), creds, ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());

	const std::string query = order_test_query(request->endpoint.target);
	const std::string tag   = "&signature=";
	const std::size_t at    = query.rfind(tag);
	ASSERT_NE(at, std::string::npos) << "nothing was signed: " << query;

	// Recomputing it here is the point: what was signed has to be exactly the
	// bytes that precede the signature, or the venue answers -1022.
	EXPECT_EQ(query.substr(at + tag.size()),
			  sign(query.substr(0, at), creds.secret));
}

TEST(BinancePlaceOrder, TheKeyTravelsInAHeaderAndNotTheQuery) {
	const auto request = place_order(order_test_limit_buy(),
									 order_test_credentials(),
									 ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());

	EXPECT_EQ(request->api_key, "test-key");
	// A key in the query would be signed into the URL and land in every proxy
	// log between here and the venue.
	EXPECT_FALSE(request->endpoint.target.contains("test-key"));
	// The secret never appears anywhere at all.
	EXPECT_FALSE(request->endpoint.target.contains("test-secret"));
}

TEST(BinancePlaceOrder, TheEnvironmentChoosesTheHost) {
	const auto sandbox = place_order(order_test_limit_buy(),
									 order_test_credentials(),
									 ORDER_TEST_TIME_MS,
									 environment::testnet);
	const auto live    = place_order(order_test_limit_buy(),
									 order_test_credentials(),
									 ORDER_TEST_TIME_MS,
									 environment::production);
	ASSERT_TRUE(sandbox.has_value());
	ASSERT_TRUE(live.has_value());

	EXPECT_EQ(sandbox->endpoint.host, "testnet.binance.vision");
	EXPECT_EQ(live->endpoint.host, "api.binance.com");
}

TEST(BinancePlaceOrder, DemoModeIsReachableAndIsNotProduction) {
	const auto demo = place_order(order_test_limit_buy(),
								  order_test_credentials(),
								  ORDER_TEST_TIME_MS,
								  environment::demo);
	ASSERT_TRUE(demo.has_value()) << describe(demo.error());

	// Fake balances, realistic depth - the deployment to measure a strategy in,
	// and emphatically not the one where a mistake costs money.
	EXPECT_EQ(demo->endpoint.host, "demo-api.binance.com");
	EXPECT_FALSE(is_production(environment::demo));
	// Still signed: demo mode takes the same credentials and the same HMAC.
	EXPECT_TRUE(demo->endpoint.target.contains("signature="));
}

TEST(BinancePlaceOrder, DefaultsToTheSandbox) {
	// The default that matters: an order sent by a caller who forgot to say
	// where must not reach production.
	const auto request = place_order(order_test_limit_buy(),
									 order_test_credentials(),
									 ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->endpoint.host, "testnet.binance.vision");
}

TEST(BinancePlaceOrder, ReportsTheWeightToDebitBeforeSending) {
	const auto request = place_order(order_test_limit_buy(),
									 order_test_credentials(),
									 ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->weight, ORDER_WEIGHT);
}

TEST(BinancePlaceOrder, AMarketOrderCarriesNoPriceOrTimeInForce) {
	outbound_order market = order_test_limit_buy();
	market.type           = order_type::MARKET;

	const auto request =
		place_order(market, order_test_credentials(), ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());

	const std::string query = order_test_query(request->endpoint.target);
	EXPECT_TRUE(query.contains("type=MARKET"));
	// Binance refuses both on a MARKET rather than ignoring them.
	EXPECT_FALSE(query.contains("price="));
	EXPECT_FALSE(query.contains("timeInForce="));
}

TEST(BinancePlaceOrder, ASellIsSpelledFromTheTakersSide) {
	outbound_order sell = order_test_limit_buy();
	sell.side           = side_t::ask;

	const auto request =
		place_order(sell, order_test_credentials(), ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());
	EXPECT_TRUE(
		order_test_query(request->endpoint.target).contains("side=SELL"));
}

TEST(BinancePlaceOrder, RefusesAnOrderWithNoCredentials) {
	EXPECT_EQ(
		place_order(order_test_limit_buy(), credentials{}, ORDER_TEST_TIME_MS)
			.error(),
		encode_error::no_credentials);
	// Half a credential is not a weaker one, it is none.
	EXPECT_EQ(place_order(order_test_limit_buy(),
						  credentials{.key = "k", .secret = ""},
						  ORDER_TEST_TIME_MS)
				  .error(),
			  encode_error::no_credentials);
}

TEST(BinancePlaceOrder, RefusesAnOrderThatCannotBeReconciled) {
	outbound_order anonymous  = order_test_limit_buy();
	anonymous.client_order_id = "";

	EXPECT_EQ(
		place_order(anonymous, order_test_credentials(), ORDER_TEST_TIME_MS)
			.error(),
		encode_error::no_client_id);
}

TEST(BinancePlaceOrder, RefusesANonPositiveQuantity) {
	outbound_order empty = order_test_limit_buy();
	empty.qty_scaled     = 0;

	EXPECT_EQ(place_order(empty, order_test_credentials(), ORDER_TEST_TIME_MS)
				  .error(),
			  encode_error::bad_quantity);
}

TEST(BinancePlaceOrder, RefusesALimitOrderWithNoPrice) {
	outbound_order unpriced = order_test_limit_buy();
	unpriced.price_scaled   = 0;

	EXPECT_EQ(
		place_order(unpriced, order_test_credentials(), ORDER_TEST_TIME_MS)
			.error(),
		encode_error::bad_price);
}

TEST(BinancePlaceOrder, RefusesAnOrderTypeItCannotSend) {
	outbound_order stop = order_test_limit_buy();
	stop.type           = order_type::STOP;

	// Nothing in this tree watches a trigger, and a venue-side stop is a
	// different product from the one the caller asked for.
	EXPECT_EQ(
		place_order(stop, order_test_credentials(), ORDER_TEST_TIME_MS).error(),
		encode_error::unsupported_type);
}

TEST(BinancePlaceOrder, RefusesATimeInForceWithNoVenueEquivalent) {
	outbound_order aon = order_test_limit_buy();
	aon.tif            = time_in_force_instruction::ALL_OR_NONE;

	// FOK is the nearest thing and it is not the same instruction - it also
	// demands immediacy. Sending it would be answering a different question.
	EXPECT_EQ(
		place_order(aon, order_test_credentials(), ORDER_TEST_TIME_MS).error(),
		encode_error::unsupported_tif);
}

TEST(BinancePlaceOrder, ACancelNamesTheOrderByOurOwnIdentifier) {
	const auto request = cancel_order(
		outbound_cancel{.symbol = "SOLUSDT", .client_order_id = "eng-42"},
		order_test_credentials(),
		ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());

	const std::string query = order_test_query(request->endpoint.target);
	// origClientOrderId, not orderId: cancelling by our own id works before the
	// placement's ack has come back with the venue's.
	EXPECT_TRUE(query.contains("origClientOrderId=eng-42")) << query;
	EXPECT_TRUE(query.contains("signature="));
}

TEST(BinancePlaceOrder, AnOpenOrdersReadIsScopedToOneListingAndSigned) {
	const auto request =
		open_orders("SOLUSDT", order_test_credentials(), ORDER_TEST_TIME_MS);
	ASSERT_TRUE(request.has_value());

	EXPECT_TRUE(request->endpoint.target.starts_with("/api/v3/openOrders?"));
	// The unscoped form costs 80 weight and returns listings we do not trade.
	EXPECT_TRUE(
		order_test_query(request->endpoint.target).contains("symbol=SOLUSDT"));
	EXPECT_TRUE(request->endpoint.target.contains("signature="));
}
