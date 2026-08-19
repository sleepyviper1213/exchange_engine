#include "binance_depth.fixture.hpp"
#include "market-data/binance/depth_feed.hpp"
#include "market-data/feed.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::market_data::feed_stop;
using exchange::market_data::binance::depth_frame_decoder;

namespace {

constexpr int PRICE_DECIMALS = 2;
constexpr int QTY_DECIMALS   = 2;

} // namespace

TEST(DepthFrameDecoder, DecodesAFrameIntoANeutralEvent) {
	depth_frame_decoder decoder(PRICE_DECIMALS, QTY_DECIMALS);

	const auto event = decoder.decode(UPDATE_JSON);

	ASSERT_TRUE(event.has_value());
	EXPECT_EQ(event->sequence.first(), 390'497'796);
	EXPECT_EQ(event->sequence.last(), 390'497'878);
	EXPECT_EQ(decoder.frames(), 1u);
	EXPECT_EQ(decoder.malformed(), 0u);
}

TEST(DepthFrameDecoder, TheEventOutlivesTheFrameItCameFrom) {
	// The documented guarantee, and the one that matters for a live feed: a
	// stream_reader hands out a view into its own buffer that dies at the next
	// read, so an event that viewed into the frame would be reading a later
	// frame's bytes by the time the reconstructor buffered it.
	depth_frame_decoder decoder(PRICE_DECIMALS, QTY_DECIMALS);
	auto event = [&] {
		const std::string frame(UPDATE_JSON);
		return decoder.decode(frame);
	}();

	ASSERT_TRUE(event.has_value());
	ASSERT_EQ(event->bids.size(), 2u);
	EXPECT_EQ(event->bids[0].price, 15345);
	EXPECT_EQ(event->bids[1].qty, 550);
}

TEST(DepthFrameDecoder, StampsTheCallersPositionOntoAFailure) {
	depth_frame_decoder decoder(PRICE_DECIMALS, QTY_DECIMALS);

	const auto event = decoder.decode("{not json at all", 7);

	ASSERT_FALSE(event.has_value());
	EXPECT_EQ(event.error().reason, feed_stop::malformed);
	// The decoder does not know whether 7 is a line or a frame index - it is
	// the caller's coordinate, carried so the caller need not re-attach it.
	EXPECT_EQ(event.error().position, 7u);
	EXPECT_FALSE(event.error().detail.empty());
	EXPECT_EQ(decoder.malformed(), 1u);
	EXPECT_EQ(decoder.frames(), 0u);
}

TEST(DepthFrameDecoder, APositionIsOptionalAndDefaultsToNone) {
	depth_frame_decoder decoder(PRICE_DECIMALS, QTY_DECIMALS);

	const auto event = decoder.decode("{not json at all");

	ASSERT_FALSE(event.has_value());
	EXPECT_EQ(event.error().position, 0u);
}

TEST(DepthFrameDecoder, KeepsDecodingAfterAMalformedFrame) {
	depth_frame_decoder decoder(PRICE_DECIMALS, QTY_DECIMALS);

	EXPECT_FALSE(decoder.decode("{not json at all", 1).has_value());
	// A live feed cannot afford a decoder that poisons itself on one bad frame:
	// the venue's next frame is the recovery, and the sequencer handles the
	// gap.
	EXPECT_TRUE(decoder.decode(UPDATE_JSON, 2).has_value());
	EXPECT_EQ(decoder.frames(), 1u);
	EXPECT_EQ(decoder.malformed(), 1u);
}

TEST(DepthFrameDecoder, ARemovalSurvivesAsAnAbsoluteZero) {
	depth_frame_decoder decoder(PRICE_DECIMALS, QTY_DECIMALS);

	const auto event = decoder.decode(UPDATE_JSON);

	ASSERT_TRUE(event.has_value());
	EXPECT_EQ(event->bids[0].qty, 0); // "0.00" - remove this price
}
