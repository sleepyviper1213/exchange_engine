#include "market_data/binance/endpoints.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::market_data::binance::depth_snapshot;
using exchange::market_data::binance::depth_snapshot_endpoint;
using exchange::market_data::binance::depth_speed;
using exchange::market_data::binance::diff_depth_stream;

// depth_snapshot - the REST /api/v3/depth endpoint.

namespace {

TEST(DepthSnapshotEndpoint, BuildsThePathWithSymbolAndLimit) {
	const auto endpoint = depth_snapshot_endpoint("SOLUSDT", 100);
	EXPECT_EQ(endpoint.host, "api.binance.com");
	EXPECT_EQ(endpoint.target, "/api/v3/depth?symbol=SOLUSDT&limit=100");
}

TEST(DepthSnapshotEndpoint, SendsTheSymbolUnchanged) {
	// Unlike a stream name, the REST symbol is case-sensitive and uppercase.
	EXPECT_EQ(depth_snapshot_endpoint("SOLUSDT", 5).target,
			  "/api/v3/depth?symbol=SOLUSDT&limit=5");
}

TEST(DepthSnapshotEndpoint, CarriesTheLimitBoundsBinanceAccepts) {
	// 5 and 5000 are the documented ends of the spot depth limit range.
	EXPECT_EQ(depth_snapshot_endpoint("BTCUSDT", 5000).target,
			  "/api/v3/depth?symbol=BTCUSDT&limit=5000");
}

} // namespace
