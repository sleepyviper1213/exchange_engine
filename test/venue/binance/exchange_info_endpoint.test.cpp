#include "venue/binance/exchange_info.hpp"

#include <gtest/gtest.h>

using exchange::venue::environment;
using exchange::venue::binance::exchange_info_endpoint;

// exchange_info_endpoint - the REST /api/v3/exchangeInfo endpoint.

namespace {

TEST(ExchangeInfoEndpoint, BuildsThePathWithTheSymbol) {
	const auto endpoint = exchange_info_endpoint("SOLUSDT");
	EXPECT_EQ(endpoint.host, "api.binance.com");
	EXPECT_EQ(endpoint.target, "/api/v3/exchangeInfo?symbol=SOLUSDT");
}

TEST(ExchangeInfoEndpoint, SendsTheSymbolUnchanged) {
	// Case-sensitive and uppercase, like the depth endpoint and unlike a stream
	// name. A lowercased symbol here comes back as an unknown-symbol refusal.
	EXPECT_EQ(exchange_info_endpoint("BTCUSDT").target,
			  "/api/v3/exchangeInfo?symbol=BTCUSDT");
}

TEST(ExchangeInfoEndpoint, IsAlwaysScopedToOneListing) {
	// The unfiltered response describes every listing on the venue and runs to
	// megabytes, at several times the rate-limit weight. There is deliberately
	// no overload that omits the symbol.
	EXPECT_TRUE(exchange_info_endpoint("SOLUSDT").target.contains("?symbol="));
}

TEST(ExchangeInfoEndpoint, DemoModeIsAThirdHostAndNotAPrefixOfProduction) {
	// The names do not derive from one another - `demo-api.binance.com` is not
	// `api.binance.com` with a prefix, and guessing either from the other is
	// the mistake host_for exists to prevent. Demo is the deployment whose
	// depth tracks the live exchange, which is what makes it worth having
	// distinct from testnet at all.
	const auto demo = exchange_info_endpoint("SOLUSDT", environment::demo);

	EXPECT_EQ(demo.host, "demo-api.binance.com");
	EXPECT_NE(demo.host, "api.binance.com");
	EXPECT_NE(demo.host, "testnet.binance.vision");
	// Same listing, same path: only the host moves.
	EXPECT_EQ(
		demo.target,
		exchange_info_endpoint("SOLUSDT", environment::production).target);
}

TEST(ExchangeInfoEndpoint, TheEnvironmentPicksTheHostAndNotThePath) {
	// The switch that must not be half-applied: a run reading production
	// reference data while trading on testnet is configured against the wrong
	// grid. Only the host moves - the same listing has the same path on both.
	const auto live =
		exchange_info_endpoint("SOLUSDT", environment::production);
	const auto sandbox =
		exchange_info_endpoint("SOLUSDT", environment::testnet);

	EXPECT_EQ(live.host, "api.binance.com");
	EXPECT_EQ(sandbox.host, "testnet.binance.vision");
	EXPECT_EQ(live.target, sandbox.target);
}

} // namespace
