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

TEST(BinanceParseOpenOrders, TheTradeHistoryAndAnOrderLookupAreRead) {
	using exchange::venue::binance::parse_my_trades;
	using exchange::venue::binance::parse_order_client_id;

	// The shape demo's /api/v3/myTrades returned for the fill that landed
	// during an exit, trimmed.
	const auto trades = parse_my_trades(
		R"([{"symbol":"SOLUSDT","id":51234,"orderId":1833899,"price":"107.49000000",)"
		R"("qty":"0.05000000","commission":"0.00005000","commissionAsset":"SOL",)"
		R"("time":1791882116123,"isBuyer":true,"isMaker":true}])");
	ASSERT_TRUE(trades.has_value());
	ASSERT_EQ(trades->size(), 1U);
	EXPECT_EQ((*trades)[0].id, 51234);
	EXPECT_EQ((*trades)[0].order_id, 1'833'899);
	EXPECT_TRUE((*trades)[0].is_buyer);
	EXPECT_EQ((*trades)[0].qty, "0.05000000");
	EXPECT_TRUE((*trades)[0].is_maker);
	EXPECT_EQ((*trades)[0].commission, "0.00005000");
	EXPECT_EQ((*trades)[0].commission_asset, "SOL");
	EXPECT_EQ((*trades)[0].time_ms, 1'791'882'116'123);
	EXPECT_FALSE(parse_my_trades(R"({"code":-1021})").has_value());

	EXPECT_EQ(parse_order_client_id(
				  R"({"orderId":1833899,"clientOrderId":"ex-3292"})"),
			  std::optional<std::string>{"ex-3292"});
	EXPECT_FALSE(parse_order_client_id("[]").has_value());
}
