#include "market_data/binance/endpoints.hpp"

#include <gtest/gtest.h>

using exchange::market_data::binance::exchange_info;

// exchange_info - the REST /api/v3/exchangeInfo endpoint.

namespace {

TEST(ExchangeInfoEndpoint, BuildsThePathWithTheSymbol) {
	const auto endpoint = exchange_info("SOLUSDT");
	EXPECT_EQ(endpoint.host, "api.binance.com");
	EXPECT_EQ(endpoint.target, "/api/v3/exchangeInfo?symbol=SOLUSDT");
}

TEST(ExchangeInfoEndpoint, SendsTheSymbolUnchanged) {
	// Case-sensitive and uppercase, like the depth endpoint and unlike a stream
	// name. A lowercased symbol here comes back as an unknown-symbol refusal.
	EXPECT_EQ(exchange_info("BTCUSDT").target,
			  "/api/v3/exchangeInfo?symbol=BTCUSDT");
}

TEST(ExchangeInfoEndpoint, IsAlwaysScopedToOneListing) {
	// The unfiltered response describes every listing on the venue and runs to
	// megabytes, at several times the rate-limit weight. There is deliberately no
	// overload that omits the symbol.
	EXPECT_NE(exchange_info("SOLUSDT").target.find("?symbol="),
			  std::string::npos);
}

} // namespace
