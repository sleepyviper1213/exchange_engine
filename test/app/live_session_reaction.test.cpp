// Reaction time: that the session measures the right interval, and says so when
// it cannot measure one at all.
//
// The other live_session suites pin the wiring - that depth reaches the book
// and fills reach the gate. This one pins the *instrumentation*, which has its
// own failure mode: a latency metric that quietly records nothing, or records
// the wrong interval, is worse than no metric, because a run then produces a
// number somebody will act on. So every case here asserts on the sample count
// as well as the value, and the two cases that must not produce a sample say
// where the message went instead.
//
// What is deliberately not asserted is an absolute latency. The interval is
// real elapsed time on a shared machine, so the only bounds worth stating are
// the ones that separate "measured from the frame" from "measured from the
// snapshot"
// - orders of magnitude apart by construction, not percentages apart.

#include "live_session.fixture.hpp"
#include "session/reaction_metrics.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>

using exchange::core::metrics::percentile;
using exchange::core::chrono::ingress_clock;
using exchange::core::chrono::ingress_time;
using exchange::market_data::book_level;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::sequence_t;
using exchange::session::reaction_metrics;

namespace {

/// @brief One diff that moves the touch, arriving at @p arrival.
///
/// Built here rather than through the shared `diff` builder because the arrival
/// stamp is the whole subject: every other suite wants it absent, and threading
/// an extra defaulted parameter through a fixture eight files use to serve one
/// would be the wrong way round.
depth_event reaction_frame(sequence_t sequence, ingress_time arrival,
						   std::int64_t bid = LIVE_TOUCH_BID,
						   std::int64_t ask = LIVE_TOUCH_ASK) {
	depth_event event = diff(sequence,
							 0,
							 std::to_array<book_level>({level(bid, 5)}),
							 std::to_array<book_level>({level(ask, 5)}));
	event.ingress     = arrival;
	return event;
}

/// @brief A two-sided seed arriving at @p arrival.
book_snapshot reaction_seed(sequence_t sequence, ingress_time arrival) {
	book_snapshot snapshot =
		seed(sequence,
			 std::to_array<book_level>({level(LIVE_TOUCH_BID, 5)}),
			 std::to_array<book_level>({level(LIVE_TOUCH_ASK, 5)}));
	snapshot.ingress = arrival;
	return snapshot;
}

/// @brief Nanoseconds, as a stamp this far in the past on the ingress clock.
///
/// A real reading offset backwards, not a fabricated one: the session subtracts
/// it from a later reading of the same clock, so it has to be a point that
/// clock actually passed through.
ingress_time reaction_ago(std::chrono::nanoseconds how_long) {
	return ingress_clock::now() - how_long;
}

} // namespace

TEST(LiveSessionReaction, RecordsOneSamplePerStampedFrame) {
	reaction_metrics metrics;
	live_session_options options;
	options.reaction = &metrics;
	live_desk desk(options);

	ASSERT_TRUE(desk.seed_book(reaction_seed(1, ingress_clock::now())));
	const std::uint64_t before = metrics.frame_reaction_ns.read().total;

	desk.frame(reaction_frame(2, ingress_clock::now()));
	desk.frame(reaction_frame(3, ingress_clock::now()));

	// One per frame, whether or not the frame moved anything - a frame the
	// strategy declined to act on still cost the time it took to decide that.
	EXPECT_EQ(metrics.frame_reaction_ns.read().total, before + 2);
	EXPECT_EQ(metrics.unstamped.load(), 0u);
}

TEST(LiveSessionReaction, CountsAnUnstampedFrameInsteadOfTimingIt) {
	// A replayed capture and a scripted event both arrive without a stamp. The
	// counter is what stops an empty distribution reading as a fast run.
	reaction_metrics metrics;
	live_session_options options;
	options.reaction = &metrics;
	live_desk desk(options);

	desk.frame(diff(2,
					0,
					std::to_array<book_level>({level(LIVE_TOUCH_BID, 5)}),
					std::to_array<book_level>({level(LIVE_TOUCH_ASK, 5)})));

	EXPECT_EQ(metrics.frame_reaction_ns.read().total, 0u);
	EXPECT_EQ(metrics.unstamped.load(), 1u);
}

TEST(LiveSessionReaction, RefusesAStampFromTheFuture) {
	// Unreachable from one monotonic clock, so it means the stamp came from a
	// different one. Counted rather than clamped to zero: a fabricated sample
	// is indistinguishable from a real one once it is in a bucket, and a floor
	// of zero would flatter the p50.
	reaction_metrics metrics;
	live_session_options options;
	options.reaction = &metrics;
	live_desk desk(options);

	desk.frame(reaction_frame(2, ingress_clock::now() + std::chrono::hours{1}));

	EXPECT_EQ(metrics.frame_reaction_ns.read().total, 0u);
	EXPECT_EQ(metrics.unstamped.load(), 1u);
}

TEST(LiveSessionReaction, KeepsFramesAndResyncsInSeparateDistributions) {
	// Pooled, the resync samples would move the p99 of the metric whose p99 is
	// the point - they are a REST round trip larger by construction.
	reaction_metrics metrics;
	live_session_options options;
	options.reaction = &metrics;
	live_desk desk(options);

	ASSERT_TRUE(desk.seed_book(reaction_seed(1, ingress_clock::now())));
	EXPECT_EQ(metrics.resync_reaction_ns.read().total, 1u);
	EXPECT_EQ(metrics.frame_reaction_ns.read().total, 0u);

	desk.frame(reaction_frame(2, ingress_clock::now()));
	EXPECT_EQ(metrics.resync_reaction_ns.read().total, 1u);
	EXPECT_EQ(metrics.frame_reaction_ns.read().total, 1u);
}

TEST(LiveSessionReaction, MeasuresAResyncFromTheOldestBufferedEvent) {
	// The case the whole ingress-on-the-event design exists for. Both frames
	// arrive while the replica is unsynced, so both wait in the reconstructor's
	// buffer; the snapshot that bridges them lands now. What the engine is
	// being told about is depth that arrived 200 ms ago, so that is what the
	// reaction is measured from - measuring from the snapshot would report
	// microseconds and call a stalled resync healthy.
	static constexpr auto kAge = std::chrono::milliseconds{200};

	reaction_metrics metrics;
	live_session_options options;
	options.reaction = &metrics;
	live_desk desk(options);

	// Before any snapshot: the sequencer is awaiting one, so these buffer.
	desk.frame(reaction_frame(5, reaction_ago(kAge)));
	desk.frame(reaction_frame(6, reaction_ago(kAge / 2)));

	ASSERT_TRUE(desk.seed_book(reaction_seed(4, ingress_clock::now())));

	const auto resync = metrics.resync_reaction_ns.read();
	ASSERT_EQ(resync.total, 1u);
	// The bucket holding the one sample. A histogram reports a bucket's upper
	// bound, so the assertion is a floor two octaves below the true value
	// rather than an equality - which is the coarseness histogram.hpp
	// documents, and it is still four orders of magnitude away from what
	// measuring from the snapshot would have produced.
	EXPECT_GT(
		resync.quantile(percentile::PMAX),
		static_cast<std::uint64_t>(std::chrono::nanoseconds{kAge / 4}.count()));
}

TEST(LiveSessionReaction, MeasuresAResyncFromItselfWhenItBridgedNothing) {
	// The ordinary case: a snapshot with an empty buffer behind it is a
	// reaction to nothing older than itself, so its own arrival is the floor
	// and the sample must be small. The pair with the case above is what proves
	// the oldest-of rule is doing work rather than always winning.
	reaction_metrics metrics;
	live_session_options options;
	options.reaction = &metrics;
	live_desk desk(options);

	ASSERT_TRUE(desk.seed_book(reaction_seed(1, ingress_clock::now())));

	const auto resync = metrics.resync_reaction_ns.read();
	ASSERT_EQ(resync.total, 1u);
	// One second, not a tight bound: this is a shared machine and the assertion
	// is here to separate "measured from now" from "measured from 200 ms ago",
	// not to police the session's speed.
	EXPECT_LT(resync.quantile(percentile::PMAX), 1'000'000'000u);
}

TEST(LiveSessionReaction, RecordsNothingWithoutAMetricsSink) {
	// The unmeasured configuration is the default, and it has to stay a
	// null-pointer check rather than a histogram nobody reads - two clock reads
	// per frame is nothing against a 100 ms diff stream and not nothing against
	// a binary feed. Nothing to assert but that the path runs, which is the
	// point: every other suite in this directory drives it this way.
	live_desk desk;
	ASSERT_TRUE(desk.seed_book(reaction_seed(1, ingress_clock::now())));
	EXPECT_EQ(desk.frame(reaction_frame(2, ingress_clock::now())),
			  exchange::market_data::sequence_action::apply);
}
