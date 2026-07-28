#include "market-data/binance/endpoints.hpp"
#include "market-data/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using exchange::market_data::binance::depth_snapshot;
using exchange::market_data::binance::depth_speed;
using exchange::market_data::binance::diff_depth_stream;

namespace {

// --------------------------------------------------------------------------
// diff_depth_stream — the <symbol>@depth WebSocket endpoint
// --------------------------------------------------------------------------

TEST(DiffDepthStream, DefaultsToTheHundredMillisecondCadence) {
	const auto endpoint = diff_depth_stream("SOLUSDT");
	EXPECT_EQ(endpoint.host, "stream.binance.com");
	EXPECT_EQ(endpoint.port, "9443");
	EXPECT_EQ(endpoint.target, "/ws/solusdt@depth@100ms");
}

TEST(DiffDepthStream, SlowCadenceOmitsTheSuffix) {
	// Binance spells the once-per-second stream as a bare <symbol>@depth; the
	// cadence appears only on the faster variant.
	const auto endpoint =
		diff_depth_stream("SOLUSDT", depth_speed::every_1000ms);
	EXPECT_EQ(endpoint.target, "/ws/solusdt@depth");
}

TEST(DiffDepthStream, LowercasesTheSymbolWhateverCaseItArrivesIn) {
	// Stream names are lowercase; the REST API is not, so callers legitimately
	// hold an uppercase symbol and must not have to lowercase it themselves.
	EXPECT_EQ(diff_depth_stream("BTCUSDT").target, "/ws/btcusdt@depth@100ms");
	EXPECT_EQ(diff_depth_stream("btcusdt").target, "/ws/btcusdt@depth@100ms");
	EXPECT_EQ(diff_depth_stream("bTcUsDt").target, "/ws/btcusdt@depth@100ms");
}

TEST(DiffDepthStream, DigitsAndEmptySymbolsPassThroughUnharmed) {
	// Edge cases: some pairs carry digits (1INCHUSDT), and an empty symbol must
	// not be turned into a malformed path by the lowercasing pass.
	EXPECT_EQ(diff_depth_stream("1INCHUSDT").target,
			  "/ws/1inchusdt@depth@100ms");
	EXPECT_EQ(diff_depth_stream("").target, "/ws/@depth@100ms");
}

// --------------------------------------------------------------------------
// depth_snapshot — the REST seed endpoint
// --------------------------------------------------------------------------

TEST(DepthSnapshotEndpoint, BuildsThePathWithSymbolAndLimit) {
	const auto endpoint = depth_snapshot("SOLUSDT", 100);
	EXPECT_EQ(endpoint.host, "api.binance.com");
	EXPECT_EQ(endpoint.target, "/api/v3/depth?symbol=SOLUSDT&limit=100");
}

TEST(DepthSnapshotEndpoint, SendsTheSymbolUnchanged) {
	// Unlike a stream name, the REST symbol is case-sensitive and uppercase.
	EXPECT_EQ(depth_snapshot("SOLUSDT", 5).target,
			  "/api/v3/depth?symbol=SOLUSDT&limit=5");
}

TEST(DepthSnapshotEndpoint, CarriesTheLimitBoundsBinanceAccepts) {
	// 5 and 5000 are the documented ends of the spot depth limit range.
	EXPECT_EQ(depth_snapshot("BTCUSDT", 5000).target,
			  "/api/v3/depth?symbol=BTCUSDT&limit=5000");
}

// --------------------------------------------------------------------------
// depth_speed — the enum's label is the wire suffix
// --------------------------------------------------------------------------

TEST(DepthSpeed, LabelMatchesTheCadenceBinanceNames) {
	EXPECT_EQ(fmt::format("{}", depth_speed::every_100ms), "100ms");
	EXPECT_EQ(fmt::format("{}", depth_speed::every_1000ms), "1000ms");
}

TEST(DepthSpeed, StreamSuffixIsBuiltFromTheLabel) {
	// Guards the invariant that ties the two together: if the label ever
	// changes, the target must change with it rather than silently diverge.
	const auto endpoint = diff_depth_stream("solusdt", depth_speed::every_100ms);
	EXPECT_EQ(endpoint.target,
			  fmt::format("/ws/solusdt@depth@{}", depth_speed::every_100ms));
}

} // namespace
