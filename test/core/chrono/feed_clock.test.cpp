#include "core/chrono/feed.hpp"

#include <gtest/gtest.h>

// The clock a replay runs on. The property throughout is that "now" is a
// function of the capture and of nothing else - no wall clock, and no way for a
// malformed stamp to move time backwards under a rate limiter.

using exchange::core::chrono::clock_view;
using exchange::core::chrono::feed_clock;

TEST(FeedClock, StartsUnstampedAtZero) {
	const feed_clock clock;
	EXPECT_FALSE(clock.stamped());
	EXPECT_EQ(clock.now_ns(), 0U);
	EXPECT_EQ(clock.first_ns(), 0U);
	EXPECT_EQ(clock.elapsed_ns(), 0U);
}

TEST(FeedClock, TakesTheStampItIsGiven) {
	feed_clock clock;
	clock.advance_to(1'700'000'000'000'000'000ULL);
	EXPECT_TRUE(clock.stamped());
	EXPECT_EQ(clock.now_ns(), 1'700'000'000'000'000'000ULL);
	EXPECT_EQ(clock.first_ns(), 1'700'000'000'000'000'000ULL);
}

TEST(FeedClock, ElapsedSpansTheFirstStampToTheLast) {
	feed_clock clock;
	clock.advance_to(1000);
	clock.advance_to(4500);
	EXPECT_EQ(clock.elapsed_ns(), 3500U);
}

// A REST depth payload carries no event time, so the seed arrives stamped zero.
// Treating that as a real reading would rewind the clock to the epoch and hand
// the gate an interval measured from 1970.
TEST(FeedClock, AnUnstampedEventDoesNotRewindTime) {
	feed_clock clock;
	clock.advance_to(9000);
	clock.advance_to(0);
	EXPECT_EQ(clock.now_ns(), 9000U);
	EXPECT_EQ(clock.regressions(), 0U) << "zero is absent, not out of order";
}

TEST(FeedClock, RefusesAndCountsAStampThatMovesTimeBackwards) {
	feed_clock clock;
	clock.advance_to(9000);
	clock.advance_to(8999);
	EXPECT_EQ(clock.now_ns(), 9000U) << "time may not run backwards";
	EXPECT_EQ(clock.regressions(), 1U);
}

TEST(FeedClock, RepeatingAStampIsNotARegression) {
	feed_clock clock;
	clock.advance_to(9000);
	clock.advance_to(9000);
	EXPECT_EQ(clock.now_ns(), 9000U);
	EXPECT_EQ(clock.regressions(), 0U)
		<< "two frames in the same nanosecond is ordinary, not malformed";
}

// The gate holds its clock by value, so the substitution only works if the view
// keeps reading the object the harness is advancing rather than a copy of it.
TEST(FeedClock, TheViewFollowsTheClockItWasBuiltOn) {
	feed_clock clock;
	const clock_view view{clock};
	EXPECT_EQ(view.now_ns(), 0U);
	clock.advance_to(1234);
	EXPECT_EQ(view.now_ns(), 1234U);
}
