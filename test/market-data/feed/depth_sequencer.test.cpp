#include "market-data/sequencer.hpp"

#include <gtest/gtest.h>


using exchange::market_data::depth_sequencer;
using exchange::market_data::sequence_action;
using exchange::market_data::sequence_t;
using exchange::market_data::sync_state;

namespace {

// A range covering exactly one sequence number.
constexpr exchange::core::util::inclusive_range<sequence_t>
at(sequence_t sequence) {
	return {sequence, sequence};
}

// --------------------------------------------------------------------------
// Before a snapshot — nothing can be judged, so nothing may be thrown away
// --------------------------------------------------------------------------

TEST(DepthSequencer, StartsUnsynced) {
	const depth_sequencer sequencer;
	EXPECT_EQ(sequencer.state(), sync_state::awaiting_snapshot);
	EXPECT_FALSE(sequencer.is_streaming());
	EXPECT_EQ(sequencer.last_sequence(), 0u);
}

TEST(DepthSequencer, BuffersEveryEventUntilSeeded) {
	depth_sequencer sequencer;
	// Whatever the numbers say: with no reference point, an event that looks
	// stale may be exactly the one the snapshot needs bridging to.
	EXPECT_EQ(sequencer.observe({100, 105}), sequence_action::buffer);
	EXPECT_EQ(sequencer.observe({1, 2}), sequence_action::buffer);
	EXPECT_EQ(sequencer.observe({500, 400}), sequence_action::buffer);
	EXPECT_EQ(sequencer.stats().buffered, 3u);
	EXPECT_EQ(sequencer.stats().gaps, 0u);
}

// --------------------------------------------------------------------------
// Seeding — the snapshot covers everything up to S, so S+1 comes next
// --------------------------------------------------------------------------

TEST(DepthSequencer, SeedStartsStreamingAtTheNextSequence) {
	depth_sequencer sequencer;
	sequencer.seed(100);
	EXPECT_EQ(sequencer.state(), sync_state::streaming);
	EXPECT_TRUE(sequencer.is_streaming());
	EXPECT_EQ(sequencer.last_sequence(), 100u);
	EXPECT_EQ(sequencer.expected_sequence(), 101u);
}

TEST(DepthSequencer, DiscardsEventsTheSnapshotAlreadyCovers) {
	depth_sequencer sequencer;
	sequencer.seed(100);
	// u <= lastUpdateId: wholly baked into the snapshot. Re-applying would
	// write back sizes the snapshot has already superseded.
	EXPECT_EQ(sequencer.observe({90, 95}), sequence_action::discard);
	EXPECT_EQ(sequencer.observe({96, 100}), sequence_action::discard);
	EXPECT_EQ(sequencer.stats().discarded, 2u);
	// Nothing was applied, so the expectation has not moved.
	EXPECT_EQ(sequencer.expected_sequence(), 101u);
}

TEST(DepthSequencer, AppliesTheEventThatBridgesTheSnapshot) {
	depth_sequencer sequencer;
	sequencer.seed(100);
	// The documented first-event rule, U <= S+1 <= u, spanning the seam.
	EXPECT_EQ(sequencer.observe({98, 104}), sequence_action::apply);
	EXPECT_EQ(sequencer.last_sequence(), 104u);
	EXPECT_EQ(sequencer.expected_sequence(), 105u);
	// It re-covered 98..100, which the snapshot already had.
	EXPECT_EQ(sequencer.stats().overlapped, 1u);
}

TEST(DepthSequencer, AppliesAnEventStartingExactlyAtTheSeam) {
	depth_sequencer sequencer;
	sequencer.seed(100);
	EXPECT_EQ(sequencer.observe({101, 103}), sequence_action::apply);
	EXPECT_EQ(sequencer.stats().overlapped, 0u); // no re-covered ids
	EXPECT_EQ(sequencer.expected_sequence(), 104u);
}

TEST(DepthSequencer, SnapshotOlderThanTheFirstEventIsAGap) {
	depth_sequencer sequencer;
	sequencer.seed(100);
	// The events covering 101..104 were never seen: the snapshot is stale
	// relative to the buffer and a newer one is needed.
	EXPECT_EQ(sequencer.observe({105, 110}), sequence_action::gap);
	EXPECT_EQ(sequencer.state(), sync_state::awaiting_snapshot);
	EXPECT_EQ(sequencer.stats().gaps, 1u);
}

// --------------------------------------------------------------------------
// Streaming — every event must resume where the last one ended
// --------------------------------------------------------------------------

TEST(DepthSequencer, AppliesAContiguousRun) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	EXPECT_EQ(sequencer.observe({1, 3}), sequence_action::apply);
	EXPECT_EQ(sequencer.observe({4, 4}), sequence_action::apply);
	EXPECT_EQ(sequencer.observe({5, 9}), sequence_action::apply);
	EXPECT_EQ(sequencer.last_sequence(), 9u);
	EXPECT_EQ(sequencer.stats().applied, 3u);
	EXPECT_EQ(sequencer.stats().gaps, 0u);
	EXPECT_EQ(sequencer.stats().overlapped, 0u);
}

TEST(DepthSequencer, DetectsASingleMissedEvent) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	ASSERT_EQ(sequencer.observe({1, 3}), sequence_action::apply);
	// 4 never arrived. Absolute sizes mean the loss leaves no trace in the
	// book, so this is the only place it can be caught.
	EXPECT_EQ(sequencer.observe({5, 6}), sequence_action::gap);
	EXPECT_EQ(sequencer.stats().gaps, 1u);
	EXPECT_EQ(sequencer.stats().applied, 1u);
}

TEST(DepthSequencer, ARepeatedEventIsDiscardedNotAGap) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	ASSERT_EQ(sequencer.observe({1, 3}), sequence_action::apply);
	EXPECT_EQ(sequencer.observe({1, 3}), sequence_action::discard);
	EXPECT_TRUE(sequencer.is_streaming()); // a duplicate is not a discontinuity
	EXPECT_EQ(sequencer.expected_sequence(), 4u);
}

TEST(DepthSequencer, AnOverlappingEventStillAppliesAndIsCounted) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	ASSERT_EQ(sequencer.observe({1, 5}), sequence_action::apply);
	// Re-sends 3..5 but also carries 6..7, so dropping it would tear a hole.
	// Applying it is safe because each level is an absolute size.
	EXPECT_EQ(sequencer.observe({3, 7}), sequence_action::apply);
	EXPECT_EQ(sequencer.last_sequence(), 7u);
	EXPECT_EQ(sequencer.stats().overlapped, 1u);
}

TEST(DepthSequencer, AnUnorderableRangeIsAGap) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	// first > last cannot be placed in the sequence at all; assuming the best
	// would risk keeping a book that silently missed whatever it covered.
	EXPECT_EQ(sequencer.observe({9, 4}), sequence_action::gap);
	EXPECT_EQ(sequencer.stats().gaps, 1u);
	EXPECT_EQ(sequencer.state(), sync_state::awaiting_snapshot);
}

// --------------------------------------------------------------------------
// Recovery
// --------------------------------------------------------------------------

TEST(DepthSequencer, AfterAGapEverythingBuffersUntilReseeded) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	ASSERT_EQ(sequencer.observe({1, 3}), sequence_action::apply);
	ASSERT_EQ(sequencer.observe({5, 6}), sequence_action::gap);
	// No further event can be trusted against a broken sequence, and none is
	// reported as a second gap — one discontinuity, one resync.
	EXPECT_EQ(sequencer.observe({7, 8}), sequence_action::buffer);
	EXPECT_EQ(sequencer.observe({9, 9}), sequence_action::buffer);
	EXPECT_EQ(sequencer.stats().gaps, 1u);
}

TEST(DepthSequencer, ReseedingRepairsAGap) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	ASSERT_EQ(sequencer.observe({1, 3}), sequence_action::apply);
	ASSERT_EQ(sequencer.observe({8, 9}), sequence_action::gap);

	sequencer.seed(9); // the newer snapshot the gap demanded
	EXPECT_TRUE(sequencer.is_streaming());
	EXPECT_EQ(sequencer.observe({10, 11}), sequence_action::apply);
	EXPECT_EQ(sequencer.stats().gaps, 1u); // still just the one
}

TEST(DepthSequencer, InvalidateForcesAResyncWithoutCountingAGap) {
	depth_sequencer sequencer;
	sequencer.seed(100);
	ASSERT_EQ(sequencer.observe({101, 102}), sequence_action::apply);

	sequencer.invalidate(); // e.g. the transport reconnected
	EXPECT_EQ(sequencer.state(), sync_state::awaiting_snapshot);
	EXPECT_EQ(sequencer.observe({103, 104}), sequence_action::buffer);
	// The sequence numbers themselves never showed a discontinuity.
	EXPECT_EQ(sequencer.stats().gaps, 0u);
	// The last number this replica held is still worth reporting.
	EXPECT_EQ(sequencer.last_sequence(), 102u);
}

TEST(DepthSequencer, ReSeedingALiveFeedJustMovesTheExpectation) {
	depth_sequencer sequencer;
	sequencer.seed(0);
	ASSERT_EQ(sequencer.observe({1, 3}), sequence_action::apply);
	// A periodic re-snapshot of a healthy feed: no gap, the older events it
	// supersedes simply drop out.
	sequencer.seed(20);
	EXPECT_EQ(sequencer.observe(at(15)), sequence_action::discard);
	EXPECT_EQ(sequencer.observe(at(21)), sequence_action::apply);
	EXPECT_EQ(sequencer.stats().gaps, 0u);
}

} // namespace
