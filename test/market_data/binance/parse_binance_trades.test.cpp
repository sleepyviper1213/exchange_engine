#include "binance_trade.fixture.hpp"
#include "market_data/binance/binance_trade.hpp"
#include "market_data/binance/trade_error.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::market_data::binance::parse_binance_trades;
using exchange::market_data::binance::trade_error;

// parse_binance_trades - a whole JSONL tape, decoded eagerly.

namespace {

TEST(ParseBinanceTrades, DecodesEveryLineInFileOrder) {
	const std::string jsonl = binance_trade_tape(3);
	const auto trades       = parse_binance_trades(jsonl,
												   BINANCE_TRADE_PRICE_DECIMALS,
												   BINANCE_TRADE_QTY_DECIMALS);
	ASSERT_TRUE(trades.has_value());
	ASSERT_EQ(trades->size(), 3u);
	// File order, and consecutive ids - the tape's only continuity signal.
	EXPECT_EQ((*trades)[0].trade_id, BINANCE_TRADE_ID);
	EXPECT_EQ((*trades)[1].trade_id, BINANCE_TRADE_ID + 1);
	EXPECT_EQ((*trades)[2].trade_id, BINANCE_TRADE_ID + 2);
}

TEST(ParseBinanceTrades, SkipsBlankLinesRatherThanCallingThemMalformed) {
	std::string jsonl = binance_trade_tape(2);
	jsonl.push_back('\n'); // a trailing blank line, as every editor leaves
	const auto trades = parse_binance_trades(jsonl, 2, 2);
	ASSERT_TRUE(trades.has_value());
	EXPECT_EQ(trades->size(), 2u);
}

TEST(ParseBinanceTrades, ToleratesCarriageReturns) {
	// A capture written on Windows and replayed on Linux carries CRLF, and the
	// trailing return is not a parse error - it is a line ending.
	std::string jsonl(BINANCE_TRADE_JSON);
	jsonl.append("\r\n");
	jsonl.append(BINANCE_TRADE_JSON);
	jsonl.append("\r\n");
	const auto trades = parse_binance_trades(jsonl, 2, 2);
	ASSERT_TRUE(trades.has_value());
	EXPECT_EQ(trades->size(), 2u);
}

TEST(ParseBinanceTrades, ReturnsAnEmptyTapeForEmptyInput) {
	const auto trades = parse_binance_trades("", 2, 2);
	ASSERT_TRUE(trades.has_value());
	EXPECT_TRUE(trades->empty());
}

TEST(ParseBinanceTrades, NamesTheOneBasedLineThatFailed) {
	// The line number is the whole value of eager decoding over a capture: the
	// caller has a file on disk and needs to be told where to look in it.
	std::string jsonl = binance_trade_tape(2);
	jsonl.append(R"({"e":"trade","E":1,"s":"S","t":9,"p":"oops",)");
	jsonl.append(R"("q":"1.00","T":1,"m":true})");
	jsonl.push_back('\n');

	const auto trades = parse_binance_trades(jsonl, 2, 2);
	ASSERT_FALSE(trades.has_value());
	EXPECT_EQ(trades.error().code, trade_error::bad_number);
	EXPECT_EQ(trades.error().line, 3u);
}

} // namespace
