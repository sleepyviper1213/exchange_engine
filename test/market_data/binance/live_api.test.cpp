#include "market_data/binance/depth_parse_error.hpp"
#include "market_data/binance/endpoints.hpp"
#include "market_data/binance/parse_depth.hpp"
#include "transport/rest.hpp"
#include "venue/binance/api_error.hpp"
#include "venue/binance/exchange_info.hpp"
#include "venue/binance/host.hpp"
#include "venue/environment.hpp"

#include <gtest/gtest.h>

#include <string>

// Conformance against the live venue - **opt-in, and never run by default**.
//
// Every test here is prefixed `DISABLED_`, so `ctest` and a bare
// `exchange_test` skip them. Run them deliberately:
//
//     exchange_test --gtest_also_run_disabled_tests
//                --gtest_filter=BinanceLiveApi.*
//
// --- why they are disabled rather than simply written ----------------------
//
// A unit suite that reaches the network stops being one. It fails on a train,
// it fails in a sandbox, it fails when the venue is having a bad afternoon, and
// each of those failures says nothing about this repository - which is the
// fastest way to teach everyone to ignore a red suite. The rest of the binance
// suites run against recorded payloads for exactly that reason.
//
// --- so what are these for ------------------------------------------------
//
// The one question a fixture cannot answer: whether the payloads we recorded
// are still the payloads the venue sends. A fixture is a photograph, and an API
// is a moving thing - Binance adds filter types, renames statuses, and changes
// what a bad request answers with. These tests are how that drift is *found*,
// run by hand when something is being changed here or when a run behaves oddly
// against the real feed.
//
// They are read-only and unauthenticated: ping, exchangeInfo, and one
// deliberately invalid symbol. No credentials, no orders, and a handful of
// requests at weight 1-10 against a 6000-per-minute budget.

// Both halves of the venue's REST surface: the depth endpoint market_data
// builds, and the reference data and error envelope venue owns.
using namespace exchange::market_data::binance;
using namespace exchange::venue::binance;
using exchange::transport::rest::get;

TEST(BinanceLiveApi, DISABLED_TheVenueIsReachable) {
	const auto body = get("api.binance.com", "/api/v3/ping");
	ASSERT_TRUE(body.has_value()) << body.error().message();
	EXPECT_EQ(body->body, "{}")
		<< "ping answers an empty object and nothing else";
}

TEST(BinanceLiveApi, DISABLED_TheRecordedGridsStillMatchTheVenues) {
	// The values the offline fixtures assert. If one of these fails, the
	// fixture in parse_exchange_info.test.cpp is a photograph of a grid that
	// has moved, and every number a run produced for that listing was quantised
	// on the old one.
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
		auto [host, target] = exchange_info_endpoint(symbol);
		const auto body     = get(std::move(host), std::move(target));
		ASSERT_TRUE(body.has_value()) << body.error().message();

		const auto grid = parse_exchange_info(body->body, symbol);
		ASSERT_TRUE(grid.has_value()) << grid.error();
		EXPECT_EQ(grid->price_decimals, price_dp);
		EXPECT_EQ(grid->qty_decimals, qty_dp);
		EXPECT_TRUE(grid->is_trading()) << "status " << grid->status;
	}
}

TEST(BinanceLiveApi,
	 DISABLED_TheStepIsStillFinerThanTheDefaultFlagWouldAssume) {
	// The bug this whole path exists for, stated as a property rather than as
	// three numbers: `serve` used to default qty_decimals to 2, and no major
	// listing has a step that coarse. If this ever passes trivially the default
	// has stopped being dangerous.
	auto [host, target] = exchange_info_endpoint("SOLUSDT");
	const auto body     = get(std::move(host), std::move(target));
	ASSERT_TRUE(body.has_value()) << body.error().message();

	const auto grid = parse_exchange_info(body->body, "SOLUSDT");
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

// --- testnet ---------------------------------------------------------------
//
// The same surface against the venue's sandbox, driven through this tree's own
// endpoint builders rather than a hand-written URL - which is the point. A
// `curl` proves Binance is up; these prove `host_for`, the endpoint builders,
// the REST client and the parsers agree with it.
//
// Still read-only and still unauthenticated. Testnet's *order entry* needs a
// key, and nothing here places one.
//
//     exchange_test --gtest_also_run_disabled_tests
//                --gtest_filter=BinanceTestnetApi.*

TEST(BinanceTestnetApi, DISABLED_TheSandboxIsReachable) {
	const auto body =
		get(std::string(host_for(exchange::venue::environment::testnet).rest),
			"/api/v3/ping");
	ASSERT_TRUE(body.has_value()) << body.error().message();
	EXPECT_EQ(body->body, "{}");
}

TEST(BinanceTestnetApi, DISABLED_TheSandboxQuotesTheSameGridAsProduction) {
	// Testnet keeps its own book, but the *listing* is meant to be the same
	// instrument. A grid that differs would mean a strategy tuned on one is
	// quantised differently on the other, which is worth knowing before it is
	// discovered by a rejected order.
	auto [host, target] =
		exchange_info_endpoint("SOLUSDT",
							   exchange::venue::environment::testnet);
	EXPECT_EQ(host, "testnet.binance.vision")
		<< "host_for must not hand back production for the sandbox";

	const auto body = get(std::move(host), std::move(target));
	ASSERT_TRUE(body.has_value()) << body.error().message();

	const auto grid = parse_exchange_info(body->body, "SOLUSDT");
	ASSERT_TRUE(grid.has_value()) << grid.error();
	EXPECT_EQ(grid->price_decimals, 2);
	EXPECT_EQ(grid->qty_decimals, 3);
	EXPECT_TRUE(grid->is_trading()) << "status " << grid->status;
}

TEST(BinanceTestnetApi, DISABLED_ADepthSnapshotParsesIntoABook) {
	// The whole read path end to end: build the endpoint, fetch it, parse it
	// with the production parser, and check the result is a sane two-sided
	// book. This is the one that would catch a payload shape drifting.
	auto [host, target] =
		depth_snapshot_endpoint("SOLUSDT",
								100,
								exchange::venue::environment::testnet);
	const auto body = get(std::move(host), std::move(target));
	ASSERT_TRUE(body.has_value()) << body.error().message();

	const auto snapshot = parse_binance_depth(body->body, 2, 3);
	ASSERT_TRUE(snapshot.has_value()) << message(snapshot.error());

	EXPECT_GT(snapshot->lastUpdateId, 0U);
	ASSERT_FALSE(snapshot->bids.empty()) << "a trading listing has bids";
	ASSERT_FALSE(snapshot->asks.empty()) << "a trading listing has asks";
	// Binance returns bids descending and asks ascending, best first, so the
	// touch is element zero of each and must not be crossed.
	EXPECT_LT(snapshot->bids.front().price, snapshot->asks.front().price)
		<< "the sandbox published a crossed book";
}
