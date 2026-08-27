#include "market_data/l2_book.hpp"
#include "market_data/normalised.hpp"
#include "market_data/reconstructor.hpp"
#include "market_data/sequencer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

using exchange::price_t;
using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::depth_reconstructor;
using exchange::market_data::reconstructor_options;
using exchange::market_data::sequence_action;
using exchange::market_data::sequence_t;
using exchange::market_data::timestamp;

namespace {

// One bid level changing at a single sequence number - enough to tell which
// events reached the book and in what order.
depth_event bid_at(sequence_t sequence, price_t price,
				   exchange::quantity_t size) {
	return depth_event{.sequence   = {sequence, sequence},
					   .event_time = timestamp{},
					   .bids       = {{price, size}}};
}

book_snapshot seed_of(sequence_t sequence) {
	return book_snapshot{.sequence   = sequence,
						 .event_time = timestamp{},
						 .bids       = {{100, 1}},
						 .asks       = {{200, 1}}};
}

// --------------------------------------------------------------------------
// The sync procedure end to end
// --------------------------------------------------------------------------

TEST(DepthReconstructor, StartsNeedingASnapshot) {
	const depth_reconstructor reconstructor;
	EXPECT_FALSE(reconstructor.is_alive());
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_EQ(reconstructor.pending(), 0u);
}

TEST(DepthReconstructor, HoldsEventsUntilASnapshotArrives) {
	depth_reconstructor reconstructor;
	EXPECT_EQ(reconstructor.on_event(bid_at(5, 100, 5)),
			  sequence_action::buffer);
	EXPECT_EQ(reconstructor.on_event(bid_at(6, 101, 6)),
			  sequence_action::buffer);
	EXPECT_EQ(reconstructor.pending(), 2u);
	// Nothing reaches the book before it has a seed to build on.
	EXPECT_EQ(reconstructor.book().depth(side_t::bid), 0u);
}

TEST(DepthReconstructor, SnapshotDrainsTheBufferAndGoesLive) {
	depth_reconstructor reconstructor;
	reconstructor.on_event(bid_at(3, 100, 3)); // predates the snapshot
	reconstructor.on_event(bid_at(4, 101, 4)); // ditto
	reconstructor.on_event(bid_at(5, 102, 5)); // the bridging event
	reconstructor.on_event(bid_at(6, 103, 6));

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(4)));
	EXPECT_TRUE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.pending(), 0u);

	const auto &book = reconstructor.book();
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 1); // the snapshot's own
	EXPECT_EQ(book.volume_at_price(101, side_t::bid), 0); // 3 and 4 were stale
	EXPECT_EQ(book.volume_at_price(102, side_t::bid), 5); // 5 and 6 replayed
	EXPECT_EQ(book.volume_at_price(103, side_t::bid), 6);
	EXPECT_EQ(reconstructor.last_sequence(), 6u);
	EXPECT_EQ(reconstructor.stats().discarded, 2u);
	EXPECT_EQ(reconstructor.stats().applied, 2u);
}

TEST(DepthReconstructor, AppliesEventsDirectlyOnceLive) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	EXPECT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);
	EXPECT_EQ(reconstructor.book().volume_at_price(105, side_t::bid), 7);
	EXPECT_EQ(reconstructor.pending(), 0u); // nothing is retained while live
}

TEST(DepthReconstructor, ASnapshotOlderThanTheBufferDoesNotGoLive) {
	depth_reconstructor reconstructor;
	reconstructor.on_event(bid_at(20, 100, 5));
	reconstructor.on_event(bid_at(21, 101, 5));

	// Nothing covers 11..19, so this snapshot cannot be bridged to the buffer.
	EXPECT_FALSE(reconstructor.on_snapshot(seed_of(10)));
	EXPECT_TRUE(reconstructor.needs_snapshot());
	// The book is cleared rather than left as a plausible-looking near-miss.
	EXPECT_EQ(reconstructor.book().depth(side_t::bid), 0u);
	// The un-bridged events are kept - a newer snapshot may still reach them.
	EXPECT_EQ(reconstructor.pending(), 2u);

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(19)));
	EXPECT_TRUE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.book().volume_at_price(100, side_t::bid), 5);
	EXPECT_EQ(reconstructor.last_sequence(), 21u);
}

// --------------------------------------------------------------------------
// Gaps
// --------------------------------------------------------------------------

TEST(DepthReconstructor, AGapClearsTheBookAndDemandsASnapshot) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);

	// 12 was lost. The book is now a replica of nothing in particular, so it
	// must not be left readable as if it were current.
	EXPECT_EQ(reconstructor.on_event(bid_at(13, 106, 8)), sequence_action::gap);
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_EQ(reconstructor.book().depth(side_t::bid), 0u);
	EXPECT_EQ(reconstructor.book().depth(side_t::ask), 0u);
	EXPECT_EQ(reconstructor.stats().gaps, 1u);
	// The event that exposed the gap is kept: the next snapshot may bridge it.
	EXPECT_EQ(reconstructor.pending(), 1u);
}

TEST(DepthReconstructor, RecoversFromAGapOnTheNextSnapshot) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);
	ASSERT_EQ(reconstructor.on_event(bid_at(13, 106, 8)), sequence_action::gap);
	ASSERT_EQ(reconstructor.on_event(bid_at(14, 107, 9)),
			  sequence_action::buffer);

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(12)));
	EXPECT_TRUE(reconstructor.is_alive());
	const auto &book = reconstructor.book();
	EXPECT_EQ(book.volume_at_price(106, side_t::bid), 8); // 13 and 14 replayed
	EXPECT_EQ(book.volume_at_price(107, side_t::bid), 9);
	EXPECT_EQ(book.volume_at_price(105, side_t::bid),
			  0); // the stale book is gone
	EXPECT_EQ(book.best_ask(), std::optional<price_t>{200}); // reseeded from 12
	EXPECT_EQ(reconstructor.stats().gaps, 1u);
}

TEST(DepthReconstructor, InvalidateDropsTheReplicaWithoutBlamingTheFeed) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);

	reconstructor.invalidate(); // e.g. the WebSocket dropped and reconnected
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_EQ(reconstructor.book().depth(side_t::bid), 0u);
	EXPECT_EQ(reconstructor.stats().gaps, 0u); // the sequence never broke
	EXPECT_EQ(reconstructor.on_event(bid_at(12, 106, 1)),
			  sequence_action::buffer);
}

// --------------------------------------------------------------------------
// The pending buffer is bounded
// --------------------------------------------------------------------------

TEST(DepthReconstructor, DropsTheOldestPendingEventsAtTheCap) {
	depth_reconstructor reconstructor{reconstructor_options{.max_pending = 2}};
	reconstructor.on_event(bid_at(1, 100, 1));
	reconstructor.on_event(bid_at(2, 101, 2));
	reconstructor.on_event(bid_at(3, 102, 3)); // evicts sequence 1

	EXPECT_EQ(reconstructor.pending(), 2u);
	EXPECT_EQ(reconstructor.dropped(), 1u);

	// Dropping from the front is safe: a snapshot that arrives this late covers
	// the evicted events anyway, so the buffer still bridges.
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(1)));
	EXPECT_EQ(reconstructor.book().volume_at_price(101, side_t::bid), 2);
	EXPECT_EQ(reconstructor.book().volume_at_price(102, side_t::bid), 3);
	EXPECT_EQ(reconstructor.last_sequence(), 3u);
}

TEST(DepthReconstructor, ZeroCapMeansUnbounded) {
	depth_reconstructor reconstructor{reconstructor_options{.max_pending = 0}};
	for (std::uint64_t sequence = 1; sequence <= 100; ++sequence)
		reconstructor.on_event(
			bid_at(sequence, static_cast<price_t>(100 + sequence), 1));
	EXPECT_EQ(reconstructor.pending(), 100u);
	EXPECT_EQ(reconstructor.dropped(), 0u);
}

// --------------------------------------------------------------------------
// Snapshots that would move a live replica backwards
// --------------------------------------------------------------------------

// Two fetches outstanding and the older one lands second. Applying it would
// overwrite the book with older depth and rewind the expected sequence, while
// leaving is_alive() true - the replica would be silently wrong until some
// later event happened to trip a gap, which on a quiet symbol could be a long
// time.
TEST(DepthReconstructor, ASnapshotOlderThanALiveReplicaIsIgnored) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(100)));
	ASSERT_EQ(reconstructor.on_event(bid_at(101, 100, 7)),
			  sequence_action::apply);
	ASSERT_EQ(reconstructor.on_event(bid_at(102, 100, 9)),
			  sequence_action::apply);

	EXPECT_TRUE(reconstructor.on_snapshot(seed_of(100))); // still live...
	EXPECT_TRUE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.last_sequence(), 102u);       // ...and not rewound
	EXPECT_EQ(reconstructor.book().volume_at_price(100, side_t::bid), 9);
	EXPECT_EQ(reconstructor.stale_snapshots(), 1u);
}

// A snapshot at exactly the current sequence adds nothing either.
TEST(DepthReconstructor, ASnapshotLevelWithTheLiveSequenceIsIgnored) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);

	EXPECT_TRUE(reconstructor.on_snapshot(seed_of(11)));
	EXPECT_EQ(reconstructor.book().volume_at_price(105, side_t::bid), 7);
	EXPECT_EQ(reconstructor.stale_snapshots(), 1u);
}

// The guard must not block the case it exists to protect: a snapshot that
// genuinely advances a live replica is still a valid periodic re-sync.
TEST(DepthReconstructor, ANewerSnapshotStillReseedsALiveReplica) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::apply);

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(50)));
	EXPECT_EQ(reconstructor.last_sequence(), 50u);
	EXPECT_EQ(reconstructor.book().volume_at_price(105, side_t::bid), 0);
	EXPECT_EQ(reconstructor.stale_snapshots(), 0u);
}

// While unsynced there is nothing to move backwards, so an old snapshot is
// judged on whether it bridges the buffer - not on its age.
TEST(DepthReconstructor, TheGuardDoesNotApplyWhileUnsynced) {
	depth_reconstructor reconstructor;
	ASSERT_EQ(reconstructor.on_event(bid_at(5, 105, 7)),
			  sequence_action::buffer);

	EXPECT_TRUE(reconstructor.on_snapshot(seed_of(4)));
	EXPECT_TRUE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.stale_snapshots(), 0u);
}

// --------------------------------------------------------------------------
// Crossed books - the consistency check sequence numbers cannot provide
// --------------------------------------------------------------------------

TEST(DepthReconstructor, AnEventThatCrossesTheBookForcesAResync) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10))); // bid 100 / ask 200

	// In sequence, well-formed, and impossible: a bid above the resting ask.
	EXPECT_EQ(reconstructor.on_event(bid_at(11, 250, 5)), sequence_action::gap);
	EXPECT_FALSE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.book().depth(side_t::bid), 0u);
	EXPECT_EQ(reconstructor.crosses(), 1u);
	// The sequence never broke, so this is not the feed losing data.
	EXPECT_EQ(reconstructor.stats().gaps, 0u);
	// Kept, like any gap-triggering event: the next snapshot may bridge it.
	EXPECT_EQ(reconstructor.pending(), 1u);
}

TEST(DepthReconstructor, ATornSnapshotThatArrivesCrossedIsRefused) {
	depth_reconstructor reconstructor;
	// A REST read caught mid-update: every sequence number is fine, the depth
	// is not.
	EXPECT_FALSE(
		reconstructor.on_snapshot(book_snapshot{.sequence   = 10,
												.event_time = timestamp{},
												.bids       = {{105, 5}},
												.asks       = {{100, 5}}}));
	EXPECT_FALSE(reconstructor.is_alive());
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_EQ(reconstructor.crosses(), 1u);
}

TEST(DepthReconstructor, ALockedBookCountsAsCrossed) {
	depth_reconstructor reconstructor;
	EXPECT_FALSE(
		reconstructor.on_snapshot(book_snapshot{.sequence   = 10,
												.event_time = timestamp{},
												.bids       = {{100, 5}},
												.asks       = {{100, 5}}}));
	EXPECT_EQ(reconstructor.crosses(), 1u);
}

TEST(DepthReconstructor, CrossesAreCountedButNotActedOnWhenDisarmed) {
	depth_reconstructor reconstructor{
		reconstructor_options{.resync_on_cross = false}};
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));

	EXPECT_EQ(reconstructor.on_event(bid_at(11, 250, 5)),
			  sequence_action::apply);
	EXPECT_TRUE(reconstructor.is_alive()); // the caller opted into trusting it
	EXPECT_EQ(reconstructor.crosses(), 1u);
	EXPECT_TRUE(reconstructor.book().is_crossed());
}

// A one-sided book has nothing to cross with, and an event that only deepens
// one side must not be mistaken for one.
TEST(DepthReconstructor, AOneSidedBookIsNotCrossed) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(
		reconstructor.on_snapshot(book_snapshot{.sequence   = 10,
												.event_time = timestamp{},
												.bids       = {{100, 5}}}));
	EXPECT_EQ(reconstructor.on_event(bid_at(11, 99999, 5)),
			  sequence_action::apply);
	EXPECT_TRUE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.crosses(), 0u);
}

// --------------------------------------------------------------------------
// Snapshot fetch bookkeeping
// --------------------------------------------------------------------------

// Without this, a caller polling needs_snapshot() per event issues one fetch
// per event for the whole round trip.
TEST(DepthReconstructor, AnAnnouncedFetchSuppressesFurtherDemands) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.needs_snapshot());

	reconstructor.snapshot_requested();
	EXPECT_FALSE(reconstructor.needs_snapshot());
	EXPECT_TRUE(reconstructor.snapshot_in_flight());

	// Events still buffer while the fetch is out; the demand stays suppressed.
	ASSERT_EQ(reconstructor.on_event(bid_at(11, 105, 7)),
			  sequence_action::buffer);
	EXPECT_FALSE(reconstructor.needs_snapshot());

	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	EXPECT_FALSE(reconstructor.snapshot_in_flight());
	EXPECT_FALSE(reconstructor.needs_snapshot()); // live now
}

// A fetch that errors must not wedge the replica dead and silent.
TEST(DepthReconstructor, AFailedFetchRestoresTheDemand) {
	depth_reconstructor reconstructor;
	reconstructor.snapshot_requested();
	ASSERT_FALSE(reconstructor.needs_snapshot());

	reconstructor.snapshot_failed();
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_FALSE(reconstructor.snapshot_in_flight());
}

// A snapshot that arrives but does not bridge leaves the demand standing.
TEST(DepthReconstructor, ASnapshotThatDoesNotBridgeReArmsTheDemand) {
	depth_reconstructor reconstructor;
	ASSERT_EQ(reconstructor.on_event(bid_at(20, 105, 7)),
			  sequence_action::buffer);
	reconstructor.snapshot_requested();

	EXPECT_FALSE(
		reconstructor.on_snapshot(seed_of(10))); // nothing covers 11..19
	EXPECT_TRUE(reconstructor.needs_snapshot());
	EXPECT_FALSE(reconstructor.snapshot_in_flight());
}

TEST(DepthReconstructor, InvalidateAbandonsAnInFlightFetch) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(seed_of(10)));
	reconstructor.snapshot_requested();

	reconstructor.invalidate(); // the transport reconnected under us
	EXPECT_FALSE(reconstructor.snapshot_in_flight());
	EXPECT_TRUE(reconstructor.needs_snapshot());
}

} // namespace
