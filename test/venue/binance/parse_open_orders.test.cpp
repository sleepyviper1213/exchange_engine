#include "venue/binance/order.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

// Reading the venue's list of what is working. The case that matters is the
// failure: a reconciliation that read an unreadable body as "nothing open"
// would retire every order it believes is working.

using exchange::venue::binance::parse_open_order_ids;

TEST(BinanceParseOpenOrders, ReadsEveryClientId) {
	// The shape testnet returned for `GET /api/v3/openOrders`, trimmed.
	const auto ids = parse_open_order_ids(
		R"([{"symbol":"SOLUSDT","orderId":1833815,"clientOrderId":"ex-1_1",)"
		R"("price":"111.00000000","status":"NEW"},)"
		R"({"symbol":"SOLUSDT","orderId":1833816,"clientOrderId":"web_abc"}])");
	ASSERT_TRUE(ids.has_value());
	EXPECT_EQ(*ids, (std::vector<std::string>{"ex-1_1", "web_abc"}));
}

TEST(BinanceParseOpenOrders, AnEmptyListIsAnAnswer) {
	const auto ids = parse_open_order_ids("[]");
	ASSERT_TRUE(ids.has_value());
	EXPECT_TRUE(ids->empty());
}

TEST(BinanceParseOpenOrders, AnUnreadableBodyIsNotAnEmptyList) {
	EXPECT_FALSE(parse_open_order_ids("").has_value());
	EXPECT_FALSE(parse_open_order_ids("<html>502</html>").has_value());
	// An error envelope is an object, not the array of orders.
	EXPECT_FALSE(parse_open_order_ids(R"({"code":-1021,"msg":"Timestamp."})")
					 .has_value());
}

TEST(BinanceParseOpenOrders, AccountBalancesAreReadAsTheVenueWroteThem) {
	using exchange::venue::binance::parse_free_balances;
	using exchange::venue::binance::parse_ticker_price;

	// The demo account's answer that explained 229 refused sells: no SOL.
	const auto held = parse_free_balances(
		R"({"canTrade":true,"balances":[)"
		R"({"asset":"BTC","free":"0.04877847","locked":"0.00000000"},)"
		R"({"asset":"USDT","free":"1142.41974837","locked":"6.02350000"}]})");
	ASSERT_TRUE(held.has_value());
	ASSERT_EQ(held->size(), 2U);
	EXPECT_EQ((*held)[1].asset, "USDT");
	EXPECT_EQ((*held)[1].free, "1142.41974837");
	EXPECT_FALSE(parse_free_balances(R"({"code":-2015})").has_value());

	EXPECT_EQ(
		parse_ticker_price(R"({"symbol":"SOLUSDT","price":"120.49000000"})"),
		std::optional<std::string>{"120.49000000"});
	EXPECT_FALSE(parse_ticker_price("[]").has_value());
}
