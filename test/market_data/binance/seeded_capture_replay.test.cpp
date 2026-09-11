#include "market_data/binance/depth_feed.hpp"
#include "market_data/feed.hpp"
#include "market_data/l2_book.hpp"
#include "market_data/normalised.hpp"
#include "market_data/reconstructor.hpp"

#include <gtest/gtest.h>

#include <string>

using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_reconstructor;
using exchange::market_data::drive;
using exchange::market_data::feed_run;
using exchange::market_data::feed_stop;
using exchange::market_data::binance::jsonl_depth_feed;

// A seeded capture driven into a reconstructor - the exact composition
// `exchange_tool replay` runs, and the one a bare apply loop gets wrong.
//
// A capture has to be started *before* its snapshot is fetched, or the frames
// that bridge the two are lost. So the first frames on the file always predate
// the seed, and what happens to them is the whole subject of this suite: they
// carry absolute sizes that are older than the snapshot's, so applying them on
// top writes stale state over correct state - and because a level's size is
// absolute rather than a delta, nothing downstream can detect that it happened.

namespace {

constexpr int SEEDED_REPLAY_PRICE_DECIMALS = 2;
constexpr int SEEDED_REPLAY_QTY_DECIMALS   = 2;

/// The seed: last update id 100, one level a side.
constexpr long long SEEDED_REPLAY_SEQUENCE = 100;

book_snapshot seeded_replay_seed() {
	return book_snapshot{.sequence   = SEEDED_REPLAY_SEQUENCE,
						 .event_time = {},
						 .ingress    = {},
						 .bids       = {{15345, 1000}},
						 .asks       = {{15346, 800}}};
}

/// One depthUpdate frame covering [first, last], touching a single bid level.
std::string seeded_replay_frame(long long first, long long last,
								const std::string &price,
								const std::string &qty) {
	return R"({"e":"depthUpdate","E":1571889248277,"s":"SOLUSDT","U":)" +
		   std::to_string(first) + R"(,"u":)" + std::to_string(last) +
		   R"(,"b":[[")" + price + R"(",")" + qty + R"("]],"a":[]})" + "\n";
}

/// Whether @p book carries a level at @p price on @p side.
bool seeded_replay_holds(const exchange::market_data::l2_book &book,
						 side_t side, long long price) {
	const auto levels =
		side == side_t::bid ? book.bid_levels() : book.ask_levels();
	for (const auto &level : levels)
		if (level.price == price) return true;
	return false;
}

// --------------------------------------------------------------------------
// The bug this composition exists to prevent
// --------------------------------------------------------------------------

TEST(SeededCaptureReplay, FramesTheSnapshotAlreadyCoversAreDiscarded) {
	// The stale frame resurrects a price the venue had already removed by the
	// time the snapshot was built. A bare apply loop puts 150.00 in the book
	// and leaves it there for the rest of the run; the sequencer discards the
	// frame unread. This is the assertion the old replay path failed.
	std::string jsonl = seeded_replay_frame(90, 95, "150.00", "5.00");
	jsonl += seeded_replay_frame(101, 105, "153.44", "1.00");

	jsonl_depth_feed feed(seeded_replay_seed(),
						  jsonl,
						  SEEDED_REPLAY_PRICE_DECIMALS,
						  SEEDED_REPLAY_QTY_DECIMALS);
	depth_reconstructor replica;

	const feed_run run = drive(feed, replica);

	EXPECT_EQ(run.snapshots, 1u);
	EXPECT_EQ(run.events, 2u);
	EXPECT_EQ(run.stop.reason, feed_stop::exhausted);
	EXPECT_EQ(replica.stats().discarded, 1u);
	EXPECT_EQ(replica.stats().applied, 1u);
	EXPECT_EQ(replica.stats().gaps, 0u);

	ASSERT_TRUE(replica.is_alive());
	EXPECT_FALSE(seeded_replay_holds(replica.book(), side_t::bid, 15000))
		<< "a frame the snapshot already covered was applied on top of it";
	EXPECT_TRUE(seeded_replay_holds(replica.book(), side_t::bid, 15344));
	EXPECT_TRUE(seeded_replay_holds(replica.book(), side_t::bid, 15345));
}

TEST(SeededCaptureReplay, AStaleFrameCannotOverwriteALevelTheSnapshotCarries) {
	// The same bug at a price that *is* in the snapshot, which is the case a
	// depth cap cannot hide: the stale size would sit at the touch.
	std::string jsonl = seeded_replay_frame(90, 95, "153.45", "99.00");
	jsonl += seeded_replay_frame(101, 105, "153.44", "1.00");

	jsonl_depth_feed feed(seeded_replay_seed(), jsonl, 2, 2);
	depth_reconstructor replica;
	drive(feed, replica);

	ASSERT_TRUE(replica.is_alive());
	const auto bids = replica.book().bid_levels();
	ASSERT_FALSE(bids.empty());
	EXPECT_EQ(bids.front().price, 15345);
	EXPECT_EQ(bids.front().qty, 1000) << "the snapshot's size was overwritten "
										 "by an older frame";
}

// --------------------------------------------------------------------------
// The rest of the sequencer's rule, through the same composition
// --------------------------------------------------------------------------

TEST(SeededCaptureReplay,
	 TheFirstAppliedFrameMustCoverTheSequenceAfterTheSeed) {
	// Nothing bridges 101, so the replica cannot be trusted and says so rather
	// than quietly starting from a hole.
	const std::string jsonl = seeded_replay_frame(105, 110, "153.44", "1.00");

	jsonl_depth_feed feed(seeded_replay_seed(), jsonl, 2, 2);
	depth_reconstructor replica;
	drive(feed, replica);

	EXPECT_EQ(replica.stats().gaps, 1u);
	EXPECT_FALSE(replica.is_alive());
	EXPECT_TRUE(replica.needs_snapshot());
}

TEST(SeededCaptureReplay,
	 AFrameStraddlingTheSeedIsAppliedAndCountedOverlapped) {
	// U below the expected id but u at or above it: the event covers what is
	// wanted while re-covering ids already seen. Absolute sizes make that
	// harmless, so it is applied - but a venue promising exact adjacency should
	// never send one, which is why it is counted.
	const std::string jsonl = seeded_replay_frame(95, 105, "153.44", "1.00");

	jsonl_depth_feed feed(seeded_replay_seed(), jsonl, 2, 2);
	depth_reconstructor replica;
	drive(feed, replica);

	EXPECT_EQ(replica.stats().applied, 1u);
	EXPECT_EQ(replica.stats().overlapped, 1u);
	EXPECT_EQ(replica.stats().gaps, 0u);
	EXPECT_TRUE(replica.is_alive());
	EXPECT_TRUE(seeded_replay_holds(replica.book(), side_t::bid, 15344));
}

TEST(SeededCaptureReplay, AnUnseededCaptureAppliesNothingAtAll) {
	// What `replay` without --snapshot now does. The old apply loop produced a
	// partial book here, which looked like an answer; this produces an empty
	// one, which is the truth - a diff feed with no snapshot has no book in it.
	std::string jsonl = seeded_replay_frame(101, 105, "153.44", "1.00");
	jsonl += seeded_replay_frame(106, 110, "153.43", "2.00");

	jsonl_depth_feed feed(jsonl, 2, 2);
	depth_reconstructor replica;
	const feed_run run = drive(feed, replica);

	EXPECT_EQ(run.events, 2u);
	EXPECT_EQ(run.snapshots, 0u);
	EXPECT_EQ(replica.stats().applied, 0u);
	EXPECT_EQ(replica.stats().buffered, 2u);
	EXPECT_FALSE(replica.is_alive());
	EXPECT_TRUE(replica.book().is_empty());
}

TEST(SeededCaptureReplay, EveryFrameOfACleanCaptureIsApplied) {
	// The control: contiguous frames starting exactly at seed + 1 leave nothing
	// discarded, nothing buffered and no gap.
	std::string jsonl = seeded_replay_frame(101, 105, "153.44", "1.00");
	jsonl += seeded_replay_frame(106, 110, "153.43", "2.00");
	jsonl += seeded_replay_frame(111, 115, "153.42", "3.00");

	jsonl_depth_feed feed(seeded_replay_seed(), jsonl, 2, 2);
	depth_reconstructor replica;
	const feed_run run = drive(feed, replica);

	EXPECT_EQ(run.events, 3u);
	EXPECT_EQ(replica.stats().applied, 3u);
	EXPECT_EQ(replica.stats().discarded, 0u);
	EXPECT_EQ(replica.stats().buffered, 0u);
	EXPECT_EQ(replica.stats().gaps, 0u);
	EXPECT_EQ(replica.last_sequence(), 115);
	EXPECT_TRUE(replica.is_alive());
}

} // namespace
