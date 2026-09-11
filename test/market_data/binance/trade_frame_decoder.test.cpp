#include "binance_trade.fixture.hpp"
#include "core/chrono/ingress.hpp"
#include "market_data/binance/trade_feed.hpp"
#include "market_data/feed.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::core::chrono::ingress_clock;
using exchange::market_data::feed_stop;
using exchange::market_data::binance::trade_frame_decoder;

// trade_frame_decoder - venue frame to neutral print, with the buffers held
// across calls and the venue's errors already mapped onto feed_status.

namespace {

TEST(TradeFrameDecoder, DecodesAFrameIntoANeutralPrint) {
	trade_frame_decoder decoder(BINANCE_TRADE_PRICE_DECIMALS,
								BINANCE_TRADE_QTY_DECIMALS);
	const auto print = decoder.decode(BINANCE_TRADE_JSON);
	ASSERT_TRUE(print.has_value());
	EXPECT_EQ(print->id, static_cast<long long>(BINANCE_TRADE_ID));
	EXPECT_EQ(print->price, BINANCE_TRADE_PRICE);
	EXPECT_EQ(print->qty, BINANCE_TRADE_QTY);
}

TEST(TradeFrameDecoder, ReusesItsBuffersAcrossManyFrames) {
	// The reason this is a class rather than a free function. Decoding the same
	// frame repeatedly must give the same answer every time - a stale buffer
	// would show up here as a second call disagreeing with the first.
	trade_frame_decoder decoder(2, 2);
	for (int i = 0; i < 64; ++i) {
		const auto print = decoder.decode(BINANCE_TRADE_JSON);
		ASSERT_TRUE(print.has_value()) << "frame " << i;
		EXPECT_EQ(print->price, BINANCE_TRADE_PRICE);
	}
	EXPECT_EQ(decoder.frames(), 64u);
	EXPECT_EQ(decoder.malformed(), 0u);
}

TEST(TradeFrameDecoder, SurvivesAFrameLongerThanAnySeenBefore) {
	// The staging buffer grows and is never shrunk, so the short-then-long and
	// long-then-short orders exercise different branches of the same policy.
	trade_frame_decoder decoder(2, 2);
	std::string padded(BINANCE_TRADE_JSON);
	padded.insert(1, std::string(4096, ' '));

	ASSERT_TRUE(decoder.decode(BINANCE_TRADE_JSON).has_value());
	ASSERT_TRUE(decoder.decode(padded).has_value());
	ASSERT_TRUE(decoder.decode(BINANCE_TRADE_JSON).has_value());
	EXPECT_EQ(decoder.frames(), 3u);
}

TEST(TradeFrameDecoder, CountsFramesAndMalformedSeparately) {
	trade_frame_decoder decoder(2, 2);
	ASSERT_TRUE(decoder.decode(BINANCE_TRADE_JSON).has_value());
	ASSERT_FALSE(decoder.decode(R"({"t":)").has_value());
	ASSERT_TRUE(decoder.decode(BINANCE_TRADE_JSON).has_value());

	EXPECT_EQ(decoder.frames(), 2u);
	EXPECT_EQ(decoder.malformed(), 1u);
}

TEST(TradeFrameDecoder, ReportsMalformedWithThePositionItWasGiven) {
	trade_frame_decoder decoder(2, 2);
	const auto print = decoder.decode(R"({"t":)", 17);
	ASSERT_FALSE(print.has_value());
	EXPECT_EQ(print.error().reason, feed_stop::malformed);
	EXPECT_EQ(print.error().position, 17u);
	// Static text, as feed_status requires - never a view into the frame, which
	// is gone by the time anybody reads the status.
	EXPECT_FALSE(print.error().detail.empty());
}

TEST(TradeFrameDecoder, StampsTheIngressTimeItIsGiven) {
	// Passed in rather than read here: this is not where the bytes arrived, and
	// reading the clock here would fold the socket read into the decode cost.
	trade_frame_decoder decoder(2, 2);
	const auto arrived = ingress_clock::now();
	const auto print   = decoder.decode(BINANCE_TRADE_JSON, 1, arrived);
	ASSERT_TRUE(print.has_value());
	EXPECT_EQ(print->ingress, arrived);
}

TEST(TradeFrameDecoder, LeavesTheStampUnsetWhenNobodySuppliesOne) {
	trade_frame_decoder decoder(2, 2);
	const auto print = decoder.decode(BINANCE_TRADE_JSON);
	ASSERT_TRUE(print.has_value());
	EXPECT_FALSE(exchange::core::chrono::has_ingress(print->ingress));
}

} // namespace
