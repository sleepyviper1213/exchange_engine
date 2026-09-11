#include "binance_trade.fixture.hpp"
#include "market_data/binance/binance_trade.hpp"
#include "market_data/binance/trade_error.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::market_data::binance::parse_binance_trade;
using exchange::market_data::binance::trade_error;

// parse_binance_trade - one `@trade` frame into the venue's own shape.

namespace {

TEST(ParseBinanceTrade, DecodesEveryFieldOfAWireOrderFrame) {
	const auto trade = parse_binance_trade(BINANCE_TRADE_JSON,
										   BINANCE_TRADE_PRICE_DECIMALS,
										   BINANCE_TRADE_QTY_DECIMALS);
	ASSERT_TRUE(trade.has_value());
	EXPECT_EQ(trade->trade_id, BINANCE_TRADE_ID);
	EXPECT_EQ(trade->event_time, BINANCE_TRADE_SENT_MS);
	EXPECT_EQ(trade->trade_time, BINANCE_TRADE_EXEC_MS);
	EXPECT_EQ(trade->price, BINANCE_TRADE_PRICE);
	EXPECT_EQ(trade->qty, BINANCE_TRADE_QTY);
	EXPECT_TRUE(trade->buyer_is_maker);
}

TEST(ParseBinanceTrade, ScalesDecimalTextByTheGivenPrecision) {
	// The same text at a different scale is a different integer, and nothing
	// about the frame says which scale is right - the listing does. Eight
	// decimals is what Binance actually publishes for most quantities.
	const auto trade = parse_binance_trade(BINANCE_TRADE_JSON, 8, 8);
	ASSERT_TRUE(trade.has_value());
	EXPECT_EQ(trade->price, 15'345'000'000);
	EXPECT_EQ(trade->qty, 1'000'000'000);
}

TEST(ParseBinanceTrade, TruncatesFractionalDigitsBeyondTheScale) {
	// A venue that publishes more precision than the listing declares is not a
	// parse failure; the extra digits are simply below the step.
	const std::string frame =
		R"({"e":"trade","E":1,"s":"S","t":2,"p":"153.4567","q":"1.999",)"
		R"("T":3,"m":false,"M":true})";
	const auto trade = parse_binance_trade(frame, 2, 2);
	ASSERT_TRUE(trade.has_value());
	EXPECT_EQ(trade->price, 15345);
	EXPECT_EQ(trade->qty, 199);
}

TEST(ParseBinanceTrade, FallsBackToTheSendTimeWhenExecutionTimeIsAbsent) {
	// T missing is not a reason to stamp the print at the epoch: a feed clock
	// driven off a 1970 timestamp jumps back fifty years and every interval
	// measured after it is nonsense. E is a millisecond late, which is not.
	const std::string frame =
		R"({"e":"trade","E":1571889248277,"s":"S","t":7,"p":"1.00",)"
		R"("q":"1.00","m":true,"M":true})";
	const auto trade = parse_binance_trade(frame, 2, 2);
	ASSERT_TRUE(trade.has_value());
	EXPECT_EQ(trade->trade_time, 1'571'889'248'277u);
	EXPECT_EQ(trade->event_time, trade->trade_time);
}

TEST(ParseBinanceTrade, ToleratesAFrameWithNoSendTime) {
	// E is the one scalar a hand-written corpus may legitimately omit.
	const std::string frame =
		R"({"t":7,"p":"1.00","q":"2.00","T":99,"m":false})";
	const auto trade = parse_binance_trade(frame, 2, 2);
	ASSERT_TRUE(trade.has_value());
	EXPECT_EQ(trade->event_time, 0u);
	EXPECT_EQ(trade->trade_time, 99u);
}

// --------------------------------------------------------------------------
// Failure paths - every category, and the field each one names
// --------------------------------------------------------------------------

TEST(ParseBinanceTrade, RejectsAFrameWithNoTradeId) {
	const std::string frame = R"({"E":1,"p":"1.00","q":"1.00","T":1,"m":true})";
	const auto trade        = parse_binance_trade(frame, 2, 2);
	ASSERT_FALSE(trade.has_value());
	EXPECT_EQ(trade.error().code, trade_error::missing_field);
	EXPECT_EQ(trade.error().context, "t");
}

TEST(ParseBinanceTrade, RejectsAFrameWithNoPrice) {
	const std::string frame = R"({"E":1,"t":2,"q":"1.00","T":1,"m":true})";
	const auto trade        = parse_binance_trade(frame, 2, 2);
	ASSERT_FALSE(trade.has_value());
	EXPECT_EQ(trade.error().code, trade_error::missing_field);
	EXPECT_EQ(trade.error().context, "p");
}

TEST(ParseBinanceTrade, RejectsAFrameWithNoMakerFlag) {
	// Without m there is no aggressor side, and guessing one would silently
	// negate every order-flow measurement taken from the tape.
	const std::string frame = R"({"E":1,"t":2,"p":"1.00","q":"1.00","T":1})";
	const auto trade        = parse_binance_trade(frame, 2, 2);
	ASSERT_FALSE(trade.has_value());
	EXPECT_EQ(trade.error().code, trade_error::missing_field);
	EXPECT_EQ(trade.error().context, "m");
}

TEST(ParseBinanceTrade, RejectsAPriceThatIsNotANumber) {
	const std::string frame =
		R"({"E":1,"t":2,"p":"not-a-price","q":"1.00","T":1,"m":true})";
	const auto trade = parse_binance_trade(frame, 2, 2);
	ASSERT_FALSE(trade.has_value());
	EXPECT_EQ(trade.error().code, trade_error::bad_number);
}

TEST(ParseBinanceTrade, RejectsAPriceSentAsANumberRatherThanAString) {
	// Binance quotes every price as a decimal string. A bare JSON number would
	// have to go through a double to be read, which is exactly the conversion
	// this parser exists to avoid.
	const std::string frame =
		R"({"E":1,"t":2,"p":153.45,"q":"1.00","T":1,"m":true})";
	const auto trade = parse_binance_trade(frame, 2, 2);
	ASSERT_FALSE(trade.has_value());
	EXPECT_EQ(trade.error().code, trade_error::missing_field);
	EXPECT_EQ(trade.error().context, "p");
}

TEST(ParseBinanceTrade, RejectsInvalidJson) {
	const auto trade = parse_binance_trade(R"({"t":)", 2, 2);
	ASSERT_FALSE(trade.has_value());
	EXPECT_EQ(trade.error().code, trade_error::invalid_json);
}

TEST(ParseBinanceTrade, RejectsAnEmptyFrame) {
	const auto trade = parse_binance_trade("", 2, 2);
	ASSERT_FALSE(trade.has_value());
	EXPECT_EQ(trade.error().code, trade_error::invalid_json);
}

} // namespace
