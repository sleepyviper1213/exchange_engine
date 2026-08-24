#include "market-data/binance/exchange_info.hpp"

#include <gtest/gtest.h>

#include <string>

// Reference data, and the one suite in this module that is about a *conformance*
// question rather than a decoding one: does what we read out of the venue's own
// description of a listing match what the venue actually publishes.
//
// The payloads below are trimmed from real /api/v3/exchangeInfo responses -
// eight-decimal padding, filters in the venue's order, extra filters we do not
// read - because a fixture that tidied the shape up would stop testing the shape
// that arrives.

using namespace exchange::market_data::binance;

namespace {

/// @brief A real SOLUSDT response, trimmed to the filters and fields read.
///
/// The tick and step differ - 2 decimals against 3 - which is the whole reason
/// this path exists. `serve` defaulted both to 2 and silently truncated every
/// size the venue published.
constexpr std::string_view SOLUSDT_INFO = R"({
  "timezone": "UTC",
  "symbols": [{
    "symbol": "SOLUSDT",
    "status": "TRADING",
    "baseAsset": "SOL",
    "quoteAsset": "USDT",
    "filters": [
      {"filterType":"PRICE_FILTER","minPrice":"0.01000000","maxPrice":"10000.00000000","tickSize":"0.01000000"},
      {"filterType":"LOT_SIZE","minQty":"0.00100000","maxQty":"90000.00000000","stepSize":"0.00100000"},
      {"filterType":"NOTIONAL","minNotional":"5.00000000"}
    ]
  }]
})";

/// @brief BTCUSDT, whose step is three decimals finer again.
constexpr std::string_view BTCUSDT_INFO = R"({
  "symbols": [{
    "symbol": "BTCUSDT",
    "status": "TRADING",
    "filters": [
      {"filterType":"PRICE_FILTER","minPrice":"0.01000000","maxPrice":"1000000.00000000","tickSize":"0.01000000"},
      {"filterType":"LOT_SIZE","minQty":"0.00001000","maxQty":"9000.00000000","stepSize":"0.00001000"}
    ]
  }]
})";

} // namespace

// --- the grid --------------------------------------------------------------

TEST(BinanceParseExchangeInfo, TheGridIsReadOffTheVenuesOwnFilters) {
	const auto grid = parse_exchange_info(SOLUSDT_INFO, "SOLUSDT");
	ASSERT_TRUE(grid.has_value()) << grid.error();

	EXPECT_EQ(grid->symbol, "SOLUSDT");
	EXPECT_EQ(grid->status, "TRADING");
	EXPECT_TRUE(grid->is_trading());
	EXPECT_EQ(grid->tick_size, "0.01");
	EXPECT_EQ(grid->step_size, "0.001");
	EXPECT_EQ(grid->price_decimals, 2);
	EXPECT_EQ(grid->qty_decimals, 3)
		<< "and this is the number the flags got wrong: a step of 0.001 read at "
		   "2 decimals truncates every level below 0.01 to nothing";
}

TEST(BinanceParseExchangeInfo, ThePaddingIsRemovedBecauseTheParserRefusesIt) {
	// Not cosmetic. parse_exact_decimal requires a decimal to be exactly
	// representable at the scale it is handed, so "0.01000000" at scale 2 comes
	// back malformed - which is how the first version of this failed to start.
	const auto grid = parse_exchange_info(SOLUSDT_INFO, "SOLUSDT");
	ASSERT_TRUE(grid.has_value());

	EXPECT_EQ(grid->tick_size.find("000000"), std::string::npos);
	EXPECT_EQ(grid->step_size.find("000000"), std::string::npos);
}

TEST(BinanceParseExchangeInfo, EachListingHasItsOwnStep) {
	const auto sol = parse_exchange_info(SOLUSDT_INFO, "SOLUSDT");
	const auto btc = parse_exchange_info(BTCUSDT_INFO, "BTCUSDT");
	ASSERT_TRUE(sol.has_value());
	ASSERT_TRUE(btc.has_value());

	EXPECT_EQ(sol->price_decimals, btc->price_decimals)
		<< "the tick happens to agree across these two";
	EXPECT_NE(sol->qty_decimals, btc->qty_decimals)
		<< "the step does not, which is why one configured number cannot serve "
		   "every listing";
	EXPECT_EQ(btc->step_size, "0.00001");
}

// --- what it refuses -------------------------------------------------------

TEST(BinanceParseExchangeInfo, AnErrorEnvelopeIsReportedAsTheVenuesRefusal) {
	// A refused request is a JSON object too. Reading it as "no symbols array"
	// would throw away the one sentence that says what to fix.
	const auto grid = parse_exchange_info(
		R"({"code":-1121,"msg":"Invalid symbol."})", "NOTAPAIR");

	ASSERT_FALSE(grid.has_value());
	EXPECT_NE(grid.error().find("Invalid symbol."), std::string::npos)
		<< "got: " << grid.error();
	EXPECT_NE(grid.error().find("-1121"), std::string::npos);
}

TEST(BinanceParseExchangeInfo, AResponseForAnotherListingIsRefused) {
	// The unfiltered endpoint returns every listing on the venue. Reading the
	// first entry of that would configure a run for whatever sorts first, which
	// is the kind of mistake that produces plausible numbers about the wrong
	// instrument.
	const auto grid = parse_exchange_info(SOLUSDT_INFO, "BTCUSDT");

	ASSERT_FALSE(grid.has_value());
	EXPECT_NE(grid.error().find("BTCUSDT"), std::string::npos) << grid.error();
}

TEST(BinanceParseExchangeInfo, AMissingPriceFilterIsRefusedByName) {
	constexpr std::string_view no_price_filter = R"({
	  "symbols": [{
	    "symbol": "SOLUSDT", "status": "TRADING",
	    "filters": [{"filterType":"LOT_SIZE","stepSize":"0.00100000"}]
	  }]
	})";

	const auto grid = parse_exchange_info(no_price_filter, "SOLUSDT");

	ASSERT_FALSE(grid.has_value());
	EXPECT_NE(grid.error().find("PRICE_FILTER"), std::string::npos)
		<< "which filter was missing is the whole content of the diagnosis: "
		   "got " << grid.error();
}

TEST(BinanceParseExchangeInfo, AMissingLotSizeIsRefusedByName) {
	constexpr std::string_view no_lot_size = R"({
	  "symbols": [{
	    "symbol": "SOLUSDT", "status": "TRADING",
	    "filters": [{"filterType":"PRICE_FILTER","tickSize":"0.01000000"}]
	  }]
	})";

	const auto grid = parse_exchange_info(no_lot_size, "SOLUSDT");

	ASSERT_FALSE(grid.has_value());
	EXPECT_NE(grid.error().find("LOT_SIZE"), std::string::npos) << grid.error();
}

TEST(BinanceParseExchangeInfo, SomethingThatIsNotJsonIsRefused) {
	// An edge proxy answering with HTML is a real response to a real request,
	// and it must not be mistaken for a venue that has no such symbol.
	const auto grid = parse_exchange_info("<html>503</html>", "SOLUSDT");
	EXPECT_FALSE(grid.has_value());
}

TEST(BinanceParseExchangeInfo, AHaltedListingIsReportedRatherThanRefused) {
	constexpr std::string_view halted = R"({
	  "symbols": [{
	    "symbol": "SOLUSDT", "status": "HALT",
	    "filters": [
	      {"filterType":"PRICE_FILTER","tickSize":"0.01000000"},
	      {"filterType":"LOT_SIZE","stepSize":"0.00100000"}
	    ]
	  }]
	})";

	const auto grid = parse_exchange_info(halted, "SOLUSDT");

	// Whether to run against a halt is the caller's decision - a capture of one
	// is a legitimate thing to want - so this reports what the venue said and
	// leaves the judgement upstream.
	ASSERT_TRUE(grid.has_value()) << grid.error();
	EXPECT_FALSE(grid->is_trading());
	EXPECT_EQ(grid->status, "HALT");
	EXPECT_EQ(grid->qty_decimals, 3) << "and the grid is still usable";
}
