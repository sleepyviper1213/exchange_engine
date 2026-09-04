#include "market_data/binance/api_error.hpp"
#include "market_data/binance/endpoints.hpp"
#include "market_data/binance/exchange_info.hpp"
#include "transport/rest.hpp"

#include <gtest/gtest.h>

#include <string>

// Conformance against the live venue - **opt-in, and never run by default**.
//
// Every test here is prefixed `DISABLED_`, so `ctest` and a bare `order_test`
// skip them. Run them deliberately:
//
//     order_test --gtest_also_run_disabled_tests
//                --gtest_filter=BinanceLiveApi.*
//
// --- why they are disabled rather than simply written ----------------------
//
// A unit suite that reaches the network stops being one. It fails on a train, it
// fails in a sandbox, it fails when the venue is having a bad afternoon, and each
// of those failures says nothing about this repository - which is the fastest way
// to teach everyone to ignore a red suite. The rest of the binance suites run
// against recorded payloads for exactly that reason.
//
// --- so what are these for ------------------------------------------------
//
// The one question a fixture cannot answer: whether the payloads we recorded are
// still the payloads the venue sends. A fixture is a photograph, and an API is a
// moving thing - Binance adds filter types, renames statuses, and changes what a
// bad request answers with. These tests are how that drift is *found*, run by
// hand when something is being changed here or when a run behaves oddly against
// the real feed.
//
// They are read-only and unauthenticated: ping, exchangeInfo, and one
// deliberately invalid symbol. No credentials, no orders, and a handful of
// requests at weight 1-10 against a 6000-per-minute budget.

using namespace exchange::market_data::binance;
using exchange::transport::rest::get;

TEST(BinanceLiveApi, DISABLED_TheVenueIsReachable) {
	const auto body = get("api.binance.com", "/api/v3/ping");
	ASSERT_TRUE(body.has_value()) << body.error().message();
	EXPECT_EQ(*body, "{}") << "ping answers an empty object and nothing else";
}

TEST(BinanceLiveApi, DISABLED_TheRecordedGridsStillMatchTheVenues) {
	// The values the offline fixtures assert. If one of these fails, the fixture
	// in parse_exchange_info.test.cpp is a photograph of a grid that has moved,
	// and every number a run produced for that listing was quantised on the old
	// one.
	struct expectation {
		const char *symbol;
		int price_decimals;
		int qty_decimals;
	};
	constexpr expectation known[] = {
		{"SOLUSDT", 2, 3},
		{"ETHUSDT", 2, 4},
		{"BTCUSDT", 2, 5},
	};

	for (const auto &[symbol, price_dp, qty_dp] : known) {
		SCOPED_TRACE(symbol);
		auto [host, target] = exchange_info(symbol);
		const auto body     = get(std::move(host), std::move(target));
		ASSERT_TRUE(body.has_value()) << body.error().message();

		const auto grid = parse_exchange_info(*body, symbol);
		ASSERT_TRUE(grid.has_value()) << grid.error();
		EXPECT_EQ(grid->price_decimals, price_dp);
		EXPECT_EQ(grid->qty_decimals, qty_dp);
		EXPECT_TRUE(grid->is_trading()) << "status " << grid->status;
	}
}

TEST(BinanceLiveApi, DISABLED_TheStepIsStillFinerThanTheDefaultFlagWouldAssume) {
	// The bug this whole path exists for, stated as a property rather than as
	// three numbers: `serve` used to default qty_decimals to 2, and no major
	// listing has a step that coarse. If this ever passes trivially the default
	// has stopped being dangerous.
	auto [host, target] = exchange_info("SOLUSDT");
	const auto body     = get(std::move(host), std::move(target));
	ASSERT_TRUE(body.has_value()) << body.error().message();

	const auto grid = parse_exchange_info(*body, "SOLUSDT");
	ASSERT_TRUE(grid.has_value()) << grid.error();
	EXPECT_GT(grid->qty_decimals, 2)
		<< "a step of " << grid->step_size
		<< " read at 2 decimals truncates every level below 0.01 to zero";
}

TEST(BinanceLiveApi, DISABLED_ABadSymbolStillAnswersTheDocumentedEnvelope) {
	// The error contract, which the offline suite asserts against a recorded
	// body. -1121 / "Invalid symbol." is what the REST docs specify.
	auto [host, target] = depth_snapshot_endpoint("NOTAPAIR", 5);
	const auto body     = get(std::move(host), std::move(target));

	ASSERT_FALSE(body.has_value()) << "the venue accepted a nonsense symbol";
	EXPECT_EQ(body.error().status, 400U);
	EXPECT_FALSE(body.error().is_retryable())
		<< "and it must be classified as permanent, or the snapshot loop will "
		   "spend rate-limit budget re-learning it";

	const auto parsed = parse_api_error(body.error().body);
	ASSERT_TRUE(parsed.has_value()) << "body was: " << body.error().body;
	EXPECT_EQ(parsed->code, -1121);
	EXPECT_EQ(parsed->msg, "Invalid symbol.");
}
