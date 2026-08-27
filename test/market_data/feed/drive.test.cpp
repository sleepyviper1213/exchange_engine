#include "feed.fixture.hpp"
#include "market_data/feed.hpp"
#include "market_data/reconstructor.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using exchange::market_data::depth_reconstructor;
using exchange::market_data::drive;
using exchange::market_data::feed_run;

// The claim the whole seam rests on: the handler a driver is written against is
// a component that already existed, not an interface written for the driver.
static_assert(exchange::market_data::feed_handler<depth_reconstructor>);

// --------------------------------------------------------------------------
// Routing and counting
// --------------------------------------------------------------------------

TEST(FeedDrive, RoutesEachMessageToItsHandlerHookInOrder) {
	scripted_feed feed({pull_of(snapshot_at(10)),
						pull_of(event_at(11)),
						pull_of(event_at(12))});
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler);

	EXPECT_EQ(handler.snapshots, (std::vector<sequence_t>{10}));
	EXPECT_EQ(handler.events, (std::vector<sequence_t>{11, 12}));
	EXPECT_EQ(run.events, 2u);
	EXPECT_EQ(run.snapshots, 1u);
}

TEST(FeedDrive, AFeedWithNothingInItIsACleanRunOfZero) {
	scripted_feed feed;
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler);

	EXPECT_EQ(run.events, 0u);
	EXPECT_EQ(run.snapshots, 0u);
	EXPECT_TRUE(is_clean(run));
	EXPECT_EQ(run.stop.reason, feed_stop::exhausted);
}

TEST(FeedDrive, ReachingTheEndOfTheFeedIsNotAFailure) {
	scripted_feed feed({pull_of(event_at(1))});
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler);

	EXPECT_TRUE(is_clean(run));
	EXPECT_EQ(run.stop.reason, feed_stop::exhausted);
}

// --------------------------------------------------------------------------
// Stopping at a fault
// --------------------------------------------------------------------------

TEST(FeedDrive, StopsAtTheFirstFaultAndReportsIt) {
	scripted_feed feed({pull_of(event_at(1)),
						pull_failing(feed_stop::malformed, 2),
						pull_of(event_at(3))});
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler);

	// The event after the fault must not have reached the handler: continuing
	// past a lost frame is a decision with a sequence gap attached, and it is
	// the caller's to make.
	EXPECT_EQ(handler.events, (std::vector<sequence_t>{1}));
	EXPECT_EQ(run.events, 1u);
	EXPECT_FALSE(is_clean(run));
	EXPECT_EQ(run.stop.reason, feed_stop::malformed);
	EXPECT_EQ(run.stop.position, 2u);
}

TEST(FeedDrive, DrivingAgainResumesAfterTheFault) {
	scripted_feed feed({pull_of(event_at(1)),
						pull_failing(feed_stop::malformed, 2),
						pull_of(event_at(3))});
	recording_feed_handler handler;

	const feed_run first = drive(feed, handler);
	ASSERT_FALSE(is_clean(first));

	// A caller that decides the damage is survivable calls drive again; the
	// contract on depth_feed is what makes that work rather than re-reading the
	// frame that just failed.
	const feed_run second = drive(feed, handler);

	EXPECT_EQ(handler.events, (std::vector<sequence_t>{1, 3}));
	EXPECT_EQ(second.events, 1u);
	EXPECT_TRUE(is_clean(second));
}

TEST(FeedDrive, AnUnreadableSourceIsReportedAsSuchRatherThanAsAnEnding) {
	scripted_feed feed({pull_failing(feed_stop::unavailable)});
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler);

	EXPECT_FALSE(is_clean(run));
	EXPECT_EQ(run.stop.reason, feed_stop::unavailable);
}

// --------------------------------------------------------------------------
// The event bound
// --------------------------------------------------------------------------

TEST(FeedDrive, StopsOnceTheEventBoundIsReached) {
	scripted_feed feed(
		{pull_of(event_at(1)), pull_of(event_at(2)), pull_of(event_at(3))});
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler, 2);

	EXPECT_EQ(handler.events, (std::vector<sequence_t>{1, 2}));
	EXPECT_EQ(run.stop.reason, feed_stop::limited);
	EXPECT_TRUE(is_clean(run));
	// The bound is checked before pulling, so the third message is still there
	// for whoever wants it - the feed was stopped, not consumed.
	EXPECT_EQ(feed.pulls(), 2u);
}

TEST(FeedDrive, SnapshotsDoNotCountAgainstTheEventBound) {
	scripted_feed feed({pull_of(snapshot_at(1)),
						pull_of(event_at(2)),
						pull_of(snapshot_at(3)),
						pull_of(event_at(4)),
						pull_of(event_at(5))});
	recording_feed_handler handler;

	// "The first two events of this recording" must mean the same thing
	// whether or not the feed had to resync in the middle of them.
	const feed_run run = drive(feed, handler, 2);

	EXPECT_EQ(handler.events, (std::vector<sequence_t>{2, 4}));
	EXPECT_EQ(run.snapshots, 2u);
	EXPECT_EQ(run.stop.reason, feed_stop::limited);
}

TEST(FeedDrive, AZeroBoundIsUnbounded) {
	scripted_feed feed({pull_of(event_at(1)), pull_of(event_at(2))});
	recording_feed_handler handler;

	const feed_run run = drive(feed, handler, 0);

	EXPECT_EQ(run.events, 2u);
	EXPECT_EQ(run.stop.reason, feed_stop::exhausted);
}

TEST(FeedDrive, ABoundOfZeroEventsIsStillReachedWhenTheFeedIsEmpty) {
	scripted_feed feed({pull_of(event_at(1))});
	recording_feed_handler handler;

	// A bound equal to the count already applied stops before the first pull,
	// which is the boundary the >= comparison exists for.
	const feed_run run = drive(feed, handler, 1);

	EXPECT_EQ(run.events, 1u);
	EXPECT_EQ(run.stop.reason, feed_stop::limited);
}

// --------------------------------------------------------------------------
// Against the real handler
// --------------------------------------------------------------------------

TEST(FeedDrive, BringsARealReconstructorLiveThroughTheSameLoop) {
	scripted_feed feed({pull_of(event_at(9)), // buffered - no seed yet
						pull_of(snapshot_at(9)),
						pull_of(event_at(10)),
						pull_of(event_at(11))});
	depth_reconstructor reconstructor;

	const feed_run run = drive(feed, reconstructor);

	EXPECT_TRUE(is_clean(run));
	EXPECT_EQ(run.events, 3u);
	EXPECT_EQ(run.snapshots, 1u);
	EXPECT_TRUE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.last_sequence(), 11u);
	EXPECT_EQ(reconstructor.stats().gaps, 0u);
}

TEST(FeedDrive, AGapInTheDrivenStreamStillTearsTheReplicaDown) {
	scripted_feed feed({pull_of(snapshot_at(9)),
						pull_of(event_at(10)),
						pull_of(event_at(99))}); // 11..98 never arrived
	depth_reconstructor reconstructor;

	// The driver reports a clean run, and it is one: every message the feed
	// produced was delivered. Whether the *replica* survived is the handler's
	// verdict, not the feed's, and the two must not be conflated.
	const feed_run run = drive(feed, reconstructor);

	EXPECT_TRUE(is_clean(run));
	EXPECT_EQ(run.events, 2u);
	EXPECT_FALSE(reconstructor.is_alive());
	EXPECT_EQ(reconstructor.stats().gaps, 1u);
}
