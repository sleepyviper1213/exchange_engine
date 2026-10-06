#include "venue/binance/order.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

// A reprice, as Binance spot has to be asked for one: withdraw one order and
// place another in a single request.
//
// The bodies the classifier is tested against are not invented. Each was the
// venue's answer on testnet to a request built to provoke it, and the comment
// above each case says which.

using exchange::side_t;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::venue::credentials;
using exchange::venue::environment;
using exchange::venue::outbound_order;
using exchange::venue::outbound_replace;
using exchange::venue::binance::cancel_replace_order;
using exchange::venue::binance::classify_replace_failure;
using exchange::venue::binance::encode_error;
using exchange::venue::binance::replace_failure;

namespace {

constexpr std::int64_t REPLACE_TEST_TIME_MS = 1'499'827'319'559;

[[nodiscard]] outbound_replace replace_test_request() {
	return outbound_replace{
		.replacement =
			outbound_order{.symbol          = "SOLUSDT",
						   .client_order_id = "ex-42_1",
						   .side            = side_t::bid,
						   .type            = order_type::LIMIT,
						   .tif =
							   time_in_force_instruction::GOOD_TILL_CANCELLED,
						   .price_scaled = 11100,
						   .price_scale  = 2,
						   .qty_scaled   = 50,
						   .qty_scale    = 3},
		.cancel_client_order_id = "ex-42"};
}

[[nodiscard]] std::string replace_test_query(const std::string &target) {
	const std::size_t at = target.find('?');
	return at == std::string::npos ? std::string{} : target.substr(at + 1);
}

} // namespace

TEST(BinanceCancelReplace, NamesBothLegsAndStopsOnFailure) {
	const auto request =
		cancel_replace_order(replace_test_request(),
							 credentials{.key = "k", .secret = "s"},
							 REPLACE_TEST_TIME_MS,
							 environment::testnet);
	ASSERT_TRUE(request.has_value());

	EXPECT_TRUE(
		request->endpoint.target.starts_with("/api/v3/order/cancelReplace?"));
	const std::string query = replace_test_query(request->endpoint.target);
	// STOP_ON_FAILURE is the safety of the whole path: if the withdrawal
	// fails, the replacement is never attempted, so a reprice cannot leave two
	// of our orders working where there was one.
	EXPECT_TRUE(query.contains("cancelReplaceMode=STOP_ON_FAILURE")) << query;
	EXPECT_TRUE(query.contains("cancelOrigClientOrderId=ex-42&")) << query;
	EXPECT_TRUE(query.contains("newClientOrderId=ex-42_1")) << query;
	EXPECT_TRUE(query.contains("price=111.00")) << query;
	EXPECT_TRUE(query.contains("quantity=0.050")) << query;
	EXPECT_TRUE(query.contains("signature=")) << query;
}

TEST(BinanceCancelReplace, TheParameterOrderIsTheOneTestnetAccepted) {
	const auto request =
		cancel_replace_order(replace_test_request(),
							 credentials{.key = "k", .secret = "s"},
							 REPLACE_TEST_TIME_MS,
							 environment::testnet);
	ASSERT_TRUE(request.has_value());

	// The signature covers these bytes as written, so the order is part of the
	// contract; this one is the order a hand-signed probe got a 200 for.
	EXPECT_TRUE(replace_test_query(request->endpoint.target)
					.starts_with("symbol=SOLUSDT&side=BUY&type=LIMIT&"
								 "cancelReplaceMode=STOP_ON_FAILURE&"
								 "timeInForce=GTC&price=111.00&quantity=0.050&"
								 "cancelOrigClientOrderId=ex-42&"
								 "newClientOrderId=ex-42_1&recvWindow="));
}

TEST(BinanceCancelReplace, RefusesToNameNoOrderToWithdraw) {
	outbound_replace request = replace_test_request();
	request.cancel_client_order_id.clear();

	const auto refused =
		cancel_replace_order(request,
							 credentials{.key = "k", .secret = "s"},
							 REPLACE_TEST_TIME_MS);
	ASSERT_FALSE(refused.has_value());
	EXPECT_EQ(refused.error(), encode_error::no_client_id);
}

// Testnet's answer to a reprice of an order that did not exist: the cancel
// half failed with -2011, so the new order was never attempted.
TEST(BinanceCancelReplace, ACancelOfAnUnknownOrderMeansTheOrderIsGone) {
	EXPECT_EQ(
		classify_replace_failure(
			R"({"code":-2022,"msg":"Order cancel-replace failed.",)"
			R"("data":{"cancelResult":"FAILURE",)"
			R"("newOrderResult":"NOT_ATTEMPTED",)"
			R"("cancelResponse":{"code":-2011,"msg":"Unknown order sent."},)"
			R"("newOrderResponse":null}})"),
		replace_failure::order_gone);
}

// Testnet's answer to a reprice to a price outside PERCENT_PRICE_BY_SIDE: a
// plain envelope, and the old order was still in the open-orders list after
// it. The venue checks the new order's filters before it cancels anything.
TEST(BinanceCancelReplace, APlainRefusalLeavesTheOldOrderWorking) {
	EXPECT_EQ(
		classify_replace_failure(
			R"({"code":-1013,"msg":"Filter failure: PERCENT_PRICE_BY_SIDE"})"),
		replace_failure::unchanged);
}

TEST(BinanceCancelReplace, ACancelThatFailedForAnotherReasonChangedNothing) {
	EXPECT_EQ(
		classify_replace_failure(
			R"({"code":-2022,"msg":"Order cancel-replace failed.",)"
			R"("data":{"cancelResult":"FAILURE",)"
			R"("newOrderResult":"NOT_ATTEMPTED",)"
			R"("cancelResponse":{"code":-1021,"msg":"Timestamp outside."},)"
			R"("newOrderResponse":null}})"),
		replace_failure::unchanged);
}

TEST(BinanceCancelReplace, APartialFailureWithdrewTheOrder) {
	EXPECT_EQ(
		classify_replace_failure(
			R"({"code":-2021,"msg":"Order cancel-replace partially failed.",)"
			R"("data":{"cancelResult":"SUCCESS",)"
			R"("newOrderResult":"FAILURE"}})"),
		replace_failure::withdrawn);
}

TEST(BinanceCancelReplace, AnUnreadableBodyIsReadAsNothingMoved) {
	// The reading that keeps the old leg's cancel addressed to an order that
	// may still be working: the cost of being wrong is a redundant cancel,
	// where the opposite reading leaves an order resting unmanaged.
	EXPECT_EQ(classify_replace_failure(""), replace_failure::unchanged);
	EXPECT_EQ(classify_replace_failure("<html>502</html>"),
			  replace_failure::unchanged);
}
