#include "market-data/binance/endpoints.hpp"
#include "market-data/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using exchange::market_data::binance::depth_snapshot;
using exchange::market_data::binance::depth_speed;
using exchange::market_data::binance::diff_depth_stream;

// depth_speed — the update cadence a stream is subscribed at.

namespace {

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
