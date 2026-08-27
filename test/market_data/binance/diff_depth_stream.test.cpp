#include "market_data/binance/endpoints.hpp"
#include "market_data/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using exchange::market_data::binance::depth_snapshot;
using exchange::market_data::binance::depth_speed;
using exchange::market_data::binance::diff_depth_stream;

// diff_depth_stream - the <symbol>@depth WebSocket endpoint.

namespace {

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

} // namespace
