#include "binance_depth.fixture.hpp"
#include "market-data/binance/depth_feed.hpp"
#include "market-data/feed.hpp"
#include "market-data/normalised.hpp"
#include "market-data/reconstructor.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>

using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::depth_reconstructor;
using exchange::market_data::drive;
using exchange::market_data::feed_run;
using exchange::market_data::feed_stop;
using exchange::market_data::binance::jsonl_depth_feed;

namespace {

constexpr int PRICE_DECIMALS = 2;
constexpr int QTY_DECIMALS   = 2;

/// The one frame the shared fixture supplies, repeated to make a capture.
std::string capture_of(int frames) {
	std::string jsonl;
	for (int i = 0; i < frames; ++i) {
		jsonl.append(UPDATE_JSON);
		jsonl.push_back('\n');
	}
	return jsonl;
}

/// The event a successful pull carries, or nullptr if it carried anything else.
const depth_event *event_in(const exchange::market_data::feed_pull &pulled) {
	if (!pulled.has_value()) return nullptr;
	return std::get_if<depth_event>(&*pulled);
}

} // namespace

// --------------------------------------------------------------------------
// Decoding
// --------------------------------------------------------------------------

TEST(JsonlDepthFeed, DecodesOneNormalisedEventPerLine) {
	const std::string jsonl = capture_of(3);
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);
	depth_reconstructor reconstructor;

	const feed_run run = drive(feed, reconstructor);

	EXPECT_EQ(run.events, 3u);
	EXPECT_EQ(run.snapshots, 0u);
	EXPECT_EQ(feed.frames(), 3u);
	EXPECT_EQ(feed.malformed(), 0u);
	EXPECT_EQ(run.stop.reason, feed_stop::exhausted);
}

TEST(JsonlDepthFeed, TheDecodedEventCarriesTheVenuesSequenceRangeAndTime) {
	const std::string jsonl = capture_of(1);
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);

	const auto pulled = feed.next();

	const depth_event *event = event_in(pulled);
	ASSERT_NE(event, nullptr);
	// U and u, normalised into one inclusive range.
	EXPECT_EQ(event->sequence.first(), 390'497'796);
	EXPECT_EQ(event->sequence.last(), 390'497'878);
	// E is milliseconds on the wire and nanoseconds after normalisation.
	EXPECT_EQ(event->event_time.count(), 1'571'889'248'277LL * 1'000'000LL);
}

TEST(JsonlDepthFeed, ARemovalSurvivesAsAnAbsoluteZeroRatherThanADroppedLevel) {
	const std::string jsonl = capture_of(1);
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);

	const auto pulled = feed.next();

	const depth_event *event = event_in(pulled);
	ASSERT_NE(event, nullptr);
	ASSERT_EQ(event->bids.size(), 2u);
	EXPECT_EQ(event->bids[0].price, 15345);
	EXPECT_EQ(event->bids[0].qty, 0); // "0.00" - remove this price
	EXPECT_EQ(event->bids[1].price, 15344);
	EXPECT_EQ(event->bids[1].qty, 550);
	ASSERT_EQ(event->asks.size(), 1u);
	EXPECT_EQ(event->asks[0].price, 15346);
	EXPECT_EQ(event->asks[0].qty, 800);
}

TEST(JsonlDepthFeed, BlankAndCarriageReturnedLinesAreNotFrames) {
	// A capture written on Windows and replayed anywhere carries CRLF, and a
	// file ends with a newline - neither is a malformed frame.
	std::string jsonl;
	jsonl.append("\n");
	jsonl.append(UPDATE_JSON).append("\r\n");
	jsonl.append("   \n");
	jsonl.append(UPDATE_JSON).append("\n");
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);
	depth_reconstructor reconstructor;

	const feed_run run = drive(feed, reconstructor);

	EXPECT_EQ(run.events, 2u);
	EXPECT_EQ(feed.malformed(), 0u);
	EXPECT_TRUE(is_clean(run));
}

TEST(JsonlDepthFeed, AFinalFrameWithNoTrailingNewlineIsStillAFrame) {
	std::string jsonl(UPDATE_JSON);
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);

	EXPECT_TRUE(feed.next().has_value());
	EXPECT_FALSE(feed.next().has_value());
	EXPECT_EQ(feed.frames(), 1u);
}

TEST(JsonlDepthFeed, AnEmptyCaptureIsExhaustedImmediately) {
	const std::string jsonl;
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);

	const auto pulled = feed.next();

	ASSERT_FALSE(pulled.has_value());
	EXPECT_EQ(pulled.error().reason, feed_stop::exhausted);
	EXPECT_EQ(feed.frames(), 0u);
}

TEST(JsonlDepthFeed, KeepsReportingExhaustionOncePastTheEnd) {
	const std::string jsonl = capture_of(1);
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);

	ASSERT_TRUE(feed.next().has_value());
	EXPECT_FALSE(feed.next().has_value());
	EXPECT_FALSE(feed.next().has_value());
}

// --------------------------------------------------------------------------
// The seed
// --------------------------------------------------------------------------

TEST(JsonlDepthFeed, HandsTheSeedOverBeforeAnyFrame) {
	const std::string jsonl = capture_of(1);
	book_snapshot seed{390'497'795, {}, {{15345, 100}}, {{15346, 100}}};
	jsonl_depth_feed feed(std::move(seed), jsonl, PRICE_DECIMALS, QTY_DECIMALS);
	ASSERT_TRUE(feed.has_pending_seed());
	depth_reconstructor reconstructor;

	const feed_run run = drive(feed, reconstructor);

	EXPECT_EQ(run.snapshots, 1u);
	EXPECT_EQ(run.events, 1u);
	EXPECT_FALSE(feed.has_pending_seed());
	// The seed covers 390497795 and the frame covers 390497796..878, so the
	// two are adjacent and the replica comes up live.
	EXPECT_TRUE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.stats().gaps, 0u);
}

TEST(JsonlDepthFeed, WithoutASeedNothingEverReachesTheBook) {
	const std::string jsonl = capture_of(2);
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);
	depth_reconstructor reconstructor;

	const feed_run run = drive(feed, reconstructor);

	EXPECT_EQ(run.events, 2u);
	EXPECT_FALSE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.pending(), 2u);
	EXPECT_TRUE(reconstructor.needs_snapshot());
}

// --------------------------------------------------------------------------
// Damage
// --------------------------------------------------------------------------

TEST(JsonlDepthFeed, AMalformedLineIsReportedWithItsOneBasedNumber) {
	std::string jsonl;
	jsonl.append(UPDATE_JSON).append("\n");
	jsonl.append("{not json at all\n");
	jsonl.append(UPDATE_JSON).append("\n");
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);

	ASSERT_TRUE(feed.next().has_value());
	const auto failed = feed.next();

	ASSERT_FALSE(failed.has_value());
	EXPECT_EQ(failed.error().reason, feed_stop::malformed);
	EXPECT_EQ(failed.error().position, 2u);
	// Static text, and it says something - an empty detail would leave the
	// operator with only the category.
	EXPECT_FALSE(failed.error().detail.empty());
	EXPECT_EQ(feed.malformed(), 1u);
}

TEST(JsonlDepthFeed, ThePullAfterAMalformedLineContinuesWithTheNextOne) {
	std::string jsonl;
	jsonl.append("{not json at all\n");
	jsonl.append(UPDATE_JSON).append("\n");
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);

	ASSERT_FALSE(feed.next().has_value());
	// Resumability is the contract that lets a caller decide a damaged capture
	// is worth continuing; without it the only choice would be to abandon the
	// whole file.
	const auto recovered = feed.next();

	ASSERT_TRUE(recovered.has_value());
	EXPECT_NE(event_in(recovered), nullptr);
	EXPECT_EQ(feed.frames(), 1u);
	EXPECT_EQ(feed.malformed(), 1u);
	EXPECT_EQ(feed.line(), 2u);
}

TEST(JsonlDepthFeed, DrivingStopsAtTheDamageAndSaysWhere) {
	std::string jsonl;
	jsonl.append(UPDATE_JSON).append("\n");
	// Well-formed JSON, but not a depthUpdate: no U, no u, no levels.
	jsonl.append(R"({"E":1})").append("\n");
	jsonl.append(UPDATE_JSON).append("\n");
	jsonl_depth_feed feed(jsonl, PRICE_DECIMALS, QTY_DECIMALS);
	depth_reconstructor reconstructor;

	const feed_run run = drive(feed, reconstructor);

	EXPECT_FALSE(is_clean(run));
	EXPECT_EQ(run.events, 1u);
	EXPECT_EQ(run.stop.reason, feed_stop::malformed);
	EXPECT_EQ(run.stop.position, 2u);
}
