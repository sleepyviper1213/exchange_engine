#include "feed.fixture.hpp"
#include "market-data/feed.hpp"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

using exchange::market_data::drive;
using exchange::market_data::feed_run;
using exchange::market_data::replay_feed;

TEST(ReplayFeed, YieldsEveryEventInOrder) {
	const std::vector<depth_event> corpus{event_at(1),
										  event_at(2),
										  event_at(3)};
	replay_feed feed(corpus);
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler);

	EXPECT_EQ(handler.events, (std::vector<sequence_t>{1, 2, 3}));
	EXPECT_TRUE(handler.snapshots.empty());
	EXPECT_TRUE(is_clean(run));
}

TEST(ReplayFeed, HandsTheSeedOverBeforeAnyEvent) {
	const book_snapshot seed = snapshot_at(7);
	const std::vector<depth_event> corpus{event_at(8), event_at(9)};
	replay_feed feed(seed, corpus);
	recording_feed_handler handler;

	drive(feed, handler);

	EXPECT_EQ(handler.snapshots, (std::vector<sequence_t>{7}));
	EXPECT_EQ(handler.events, (std::vector<sequence_t>{8, 9}));
}

TEST(ReplayFeed, AnEmptyCorpusIsExhaustedImmediately) {
	replay_feed feed;

	const auto pulled = feed.next();

	ASSERT_FALSE(pulled.has_value());
	EXPECT_EQ(pulled.error().reason, feed_stop::exhausted);
	EXPECT_EQ(feed.position(), 0u);
}

TEST(ReplayFeed, KeepsReportingExhaustionOncePastTheEnd) {
	const std::vector<depth_event> corpus{event_at(1)};
	replay_feed feed(corpus);

	ASSERT_TRUE(feed.next().has_value());
	// Required of every feed: an exhausted one stays exhausted rather than
	// resuming, which is what lets a caller distinguish "ended" from "faulted"
	// and retry only the second.
	EXPECT_FALSE(feed.next().has_value());
	EXPECT_FALSE(feed.next().has_value());
}

TEST(ReplayFeed, PositionTracksTheEventsYieldedSoFar) {
	const std::vector<depth_event> corpus{event_at(1), event_at(2)};
	replay_feed feed(corpus);

	EXPECT_EQ(feed.position(), 0u);
	(void)feed.next();
	EXPECT_EQ(feed.position(), 1u);
	(void)feed.next();
	EXPECT_EQ(feed.position(), 2u);
}

TEST(ReplayFeed, RewindReplaysTheCorpusIncludingItsSeed) {
	const book_snapshot seed = snapshot_at(7);
	const std::vector<depth_event> corpus{event_at(8)};
	replay_feed feed(seed, corpus);
	recording_feed_handler first;
	drive(feed, first);

	feed.rewind();
	recording_feed_handler second;
	drive(feed, second);

	// The whole reason the feed copies rather than moves: the corpus survives
	// its own replay, so a benchmark can drive it round after round.
	EXPECT_EQ(second.snapshots, first.snapshots);
	EXPECT_EQ(second.events, first.events);
}

TEST(ReplayFeed, TheEventItYieldsCarriesTheCorpusLevels) {
	const std::vector<depth_event> corpus{event_at(4, 153, 20)};
	replay_feed feed(corpus);

	const auto pulled = feed.next();

	ASSERT_TRUE(pulled.has_value());
	const auto *event = std::get_if<depth_event>(&*pulled);
	ASSERT_NE(event, nullptr);
	ASSERT_EQ(event->bids.size(), 1u);
	EXPECT_EQ(event->bids.front().price, 153);
	EXPECT_EQ(event->bids.front().qty, 20);
	// Copied out, so the corpus still holds its own.
	EXPECT_EQ(corpus.front().bids.size(), 1u);
}
