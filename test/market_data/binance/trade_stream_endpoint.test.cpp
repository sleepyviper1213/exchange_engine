#include "market_data/binance/endpoints.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::market_data::binance::trade_stream;

// trade_stream - the <symbol>@trade WebSocket endpoint.

namespace {

TEST(TradeStreamEndpoint, ResolvesToTheStreamHostAndPort) {
	const auto endpoint = trade_stream("SOLUSDT");
	EXPECT_EQ(endpoint.host, "stream.binance.com");
	EXPECT_EQ(endpoint.port, "9443");
	EXPECT_EQ(endpoint.target, "/ws/solusdt@trade");
}

TEST(TradeStreamEndpoint, CarriesNoCadenceSuffix) {
	// Unlike the depth stream, the tape has no cadence to choose: the venue
	// pushes a message per fill rather than on a timer. A suffix here would be
	// a subscription to a stream that does not exist.
	const auto endpoint = trade_stream("SOLUSDT");
	EXPECT_EQ(endpoint.target.find("@trade@"), std::string::npos);
}

TEST(TradeStreamEndpoint, LowercasesTheSymbolWhateverCaseItArrivesIn) {
	// Stream names are lowercase; the REST API is not, so callers legitimately
	// hold an uppercase symbol and must not have to lowercase it themselves.
	EXPECT_EQ(trade_stream("BTCUSDT").target, "/ws/btcusdt@trade");
	EXPECT_EQ(trade_stream("btcusdt").target, "/ws/btcusdt@trade");
	EXPECT_EQ(trade_stream("bTcUsDt").target, "/ws/btcusdt@trade");
}

TEST(TradeStreamEndpoint, DigitsAndEmptySymbolsPassThroughUnharmed) {
	EXPECT_EQ(trade_stream("1INCHUSDT").target, "/ws/1inchusdt@trade");
	EXPECT_EQ(trade_stream("").target, "/ws/@trade");
}

} // namespace
