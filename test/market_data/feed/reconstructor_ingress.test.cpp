// What a resync's reaction time is measured from.
//
// The reconstructor's own suite pins the sync procedure; this one pins the
// single fact a latency measurement needs from it: after a snapshot bridges
// buffered events, the oldest of those events is what the process is really
// reacting to, and its arrival time is a whole REST round trip in the past.
// Measuring from the snapshot's own arrival would time the cheap half.

#include "market_data/normalised.hpp"
#include "market_data/reconstructor.hpp"
#include "market_data/sequencer.hpp"

#include <gtest/gtest.h>

#include <chrono>

using exchange::core::chrono::has_ingress;
using exchange::core::chrono::ingress_clock;
using exchange::core::chrono::ingress_time;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::depth_reconstructor;
using exchange::market_data::sequence_action;
using exchange::market_data::sequence_t;
using exchange::market_data::timestamp;

namespace {

/// A stamp @p at nanoseconds into this clock's epoch. Fabricated rather than
/// read, because the whole point is to assert *which* stamp came back and a
/// real reading is not distinguishable from the next one.
ingress_time recon_stamp(std::int64_t at) {
	return ingress_time{ingress_clock::duration{at}};
}

/// One bid level at @p sequence, arriving at @p at.
depth_event recon_event_at(sequence_t sequence, std::int64_t at) {
	return depth_event{.sequence   = {sequence, sequence},
					   .event_time = timestamp{},
					   .ingress    = recon_stamp(at),
					   .bids       = {{100, 5}}};
}

book_snapshot recon_seed_at(sequence_t sequence, std::int64_t at) {
	return book_snapshot{.sequence   = sequence,
						 .event_time = timestamp{},
						 .ingress    = recon_stamp(at),
						 .bids       = {{100, 1}},
						 .asks       = {{200, 1}}};
}

} // namespace

TEST(DepthReconstructorIngress, ReportsNothingReplayedBeforeAnySnapshot) {
	const depth_reconstructor reconstructor;
	EXPECT_FALSE(has_ingress(reconstructor.last_replay_ingress()));
}

TEST(DepthReconstructorIngress, ReportsNothingWhenASnapshotBridgedNoEvents) {
	depth_reconstructor reconstructor;
	ASSERT_TRUE(reconstructor.on_snapshot(recon_seed_at(4, 900)));
	EXPECT_FALSE(has_ingress(reconstructor.last_replay_ingress()));
}

TEST(DepthReconstructorIngress, KeepsABufferedEventsOwnArrivalTime) {
	// The behaviour the whole field exists for. Both events arrive while the
	// replica is unsynced, so both wait in the buffer; the snapshot lands much
	// later and replays them. The reaction the caller then makes is a reaction
	// to depth that arrived at 100, not at 900.
	depth_reconstructor reconstructor;
	ASSERT_EQ(reconstructor.on_event(recon_event_at(5, 100)),
			  sequence_action::buffer);
	ASSERT_EQ(reconstructor.on_event(recon_event_at(6, 200)),
			  sequence_action::buffer);
	ASSERT_EQ(reconstructor.pending(), 2u);

	ASSERT_TRUE(reconstructor.on_snapshot(recon_seed_at(4, 900)));

	EXPECT_EQ(reconstructor.last_replay_ingress(), recon_stamp(100));
}

TEST(DepthReconstructorIngress, SkipsEventsTheSnapshotAlreadyCovered) {
	// A buffered event at or below the snapshot's sequence is dropped rather
	// than applied, so it is not something the caller reacted to and must not
	// set the floor. Only the first *applied* event counts.
	depth_reconstructor reconstructor;
	ASSERT_EQ(reconstructor.on_event(recon_event_at(3, 100)),
			  sequence_action::buffer);
	ASSERT_EQ(reconstructor.on_event(recon_event_at(4, 200)),
			  sequence_action::buffer);
	ASSERT_EQ(reconstructor.on_event(recon_event_at(5, 300)),
			  sequence_action::buffer);

	ASSERT_TRUE(reconstructor.on_snapshot(recon_seed_at(4, 900)));

	EXPECT_EQ(reconstructor.last_replay_ingress(), recon_stamp(300));
}

TEST(DepthReconstructorIngress, DescribesOnlyTheMostRecentSnapshot) {
	// Reset per call, so a caller reading it after a snapshot that bridged
	// nothing does not attribute an earlier resync's lateness to this one.
	depth_reconstructor reconstructor;
	ASSERT_EQ(reconstructor.on_event(recon_event_at(5, 100)),
			  sequence_action::buffer);
	ASSERT_TRUE(reconstructor.on_snapshot(recon_seed_at(4, 900)));
	ASSERT_EQ(reconstructor.last_replay_ingress(), recon_stamp(100));

	// A later, higher snapshot with an empty buffer behind it.
	ASSERT_TRUE(reconstructor.on_snapshot(recon_seed_at(20, 1900)));
	EXPECT_FALSE(has_ingress(reconstructor.last_replay_ingress()));
}

TEST(DepthReconstructorIngress, IsUnstampedWhenTheEventsWere) {
	// An offline corpus carries no arrival times, and the replay path must say
	// so rather than inventing one - otherwise a backtest reports a reaction
	// time it never had. @see binance::jsonl_depth_feed::next
	depth_reconstructor reconstructor;
	ASSERT_EQ(reconstructor.on_event(
				  depth_event{.sequence = {5, 5}, .bids = {{100, 5}}}),
			  sequence_action::buffer);
	ASSERT_TRUE(reconstructor.on_snapshot(
		book_snapshot{.sequence = 4, .bids = {{100, 1}}, .asks = {{200, 1}}}));
	EXPECT_FALSE(has_ingress(reconstructor.last_replay_ingress()));
}
