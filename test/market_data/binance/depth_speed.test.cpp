#include "market_data/binance/depth_speed.hpp"

#include "market_data/binance/endpoints.hpp"
#include "market_data/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <magic_enum/magic_enum.hpp>

#include <optional>
#include <string>

using exchange::market_data::binance::depth_snapshot;
using exchange::market_data::binance::depth_speed;
using exchange::market_data::binance::diff_depth_stream;

// depth_speed - the update cadence a stream is subscribed at.

namespace {

TEST(DepthSpeed, LabelMatchesTheCadenceBinanceNames) {
	EXPECT_EQ(fmt::format("{}", depth_speed::every_100ms), "100ms");
	EXPECT_EQ(fmt::format("{}", depth_speed::every_1000ms), "1000ms");
}

TEST(DepthSpeed, ParsesTheCadenceItPrints) {
	// One name drives both directions, so the CLI text and the wire text are
	// the same text - a cadence cannot be accepted on input and then subscribed
	// to under a different spelling.
	EXPECT_EQ(magic_enum::enum_cast<depth_speed>("100ms"),
			  depth_speed::every_100ms);
	EXPECT_EQ(magic_enum::enum_cast<depth_speed>("1000ms"),
			  depth_speed::every_1000ms);
	for (const auto speed :
		 {depth_speed::every_100ms, depth_speed::every_1000ms}) {
		EXPECT_EQ(magic_enum::enum_cast<depth_speed>(fmt::format("{}", speed)),
				  speed);
	}
}

TEST(DepthSpeed, RejectsACadenceBinanceDoesNotPublish) {
	// The enum is only two values wide, so a bad --speed has to come back as a
	// failure rather than as the nearest cadence.
	EXPECT_EQ(magic_enum::enum_cast<depth_speed>("10ms"), std::nullopt);
	EXPECT_EQ(magic_enum::enum_cast<depth_speed>("100"), std::nullopt);
	EXPECT_EQ(magic_enum::enum_cast<depth_speed>(""), std::nullopt);
	EXPECT_EQ(magic_enum::enum_cast<depth_speed>("every_100ms"), std::nullopt);
}

TEST(DepthSpeed, StreamSuffixIsBuiltFromTheLabel) {
	// Guards the invariant that ties the two together: if the label ever
	// changes, the target must change with it rather than silently diverge.
	const auto endpoint =
		diff_depth_stream("solusdt", depth_speed::every_100ms);
	EXPECT_EQ(endpoint.target,
			  fmt::format("/ws/solusdt@depth@{}", depth_speed::every_100ms));
}

} // namespace
