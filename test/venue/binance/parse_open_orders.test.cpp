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
