// The modelled network on the live path: does a command actually wait, and does
// anything hand it over when its time comes.
//
// The second half is the whole point. `backtest::wire` has modelled the delay
// since it landed, and nothing was wrong with the model - what a live run had
// no answer for is *who wakes up*. Offline the settle loop advances market time
// and so is awake at every moment a command comes due; wall-clock time passes
// with nobody looking. These cases drive the hand that looks. @see latency_pipe

#include "live_session.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace exchange;
using exchange::market_data::book_level;

/// @brief A millisecond of flight time, which is also about the resolution a
///        real steady timer can deliver on. @see serve.cpp's deliver_on_time
constexpr std::uint64_t LIVE_FLIGHT_NS = 1'000'000;

/// @brief Options whose only departure from the default is a modelled wire.
live_session_options delayed(std::uint64_t flight_ns = LIVE_FLIGHT_NS) {
	live_session_options options;
	options.latency.order_entry_ns = flight_ns;
	return options;
}

// --- the default, which must be the chain as it always was ----------------

TEST(AppLiveSessionLatency, NoLatencyBuildsNoWireAtAll) {
	live_desk desk;
	ASSERT_TRUE(desk.seed_touch());

	EXPECT_FALSE(desk.session().pipe().is_modelled());
	EXPECT_EQ(desk.session().pipe().in_flight(), 0U);
	// The quotes are in the book already, in the frame that wrote them - which
	// is the behaviour every other suite in this directory measures.
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID + 1);
	EXPECT_EQ(desk.book().best_ask(), LIVE_TOUCH_ASK - 1);
	EXPECT_EQ(desk.deliver(LIVE_FLIGHT_NS), 0U)
		<< "there is nothing in flight to deliver, however far the clock moves";
}

// --- a wire that holds ----------------------------------------------------

TEST(AppLiveSessionLatency, OurOrdersWaitAndTheVenuesMirroredDepthDoesNot) {
	// The distinction the pipe draws, and the one case that would silently
	// invalidate every number here if it were drawn wrong: the two quotes are
	// ours and go on the wire, while the two ADDs that mirror the venue's own
	// depth into our book are not ours to delay. @see
	// latency_pipe::submit_range
	live_desk desk(delayed());
	ASSERT_TRUE(desk.seed_touch());

	ASSERT_TRUE(desk.session().pipe().is_modelled());
	EXPECT_EQ(desk.session().quoter().quotes(), 2U)
		<< "the strategy has decided and written; that part is unchanged";
	EXPECT_EQ(desk.session().pipe().in_flight(), 2U)
		<< "the two quotes, and only the two quotes";

	// The venue's liquidity is in the book already, so the engine's view of the
	// market is not behind the replica the strategy quoted against.
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID);
	EXPECT_EQ(desk.book().best_ask(), LIVE_TOUCH_ASK);
	EXPECT_EQ(desk.session().gate().working_orders(), 2U)
		<< "the gate is holding ours as working, wire or no wire";
}

TEST(AppLiveSessionLatency, DeliveringBeforeItIsDueChangesNothing) {
	live_desk desk(delayed());
	ASSERT_TRUE(desk.seed_touch());

	// The timer is allowed to fire early and often - it has to be, since it
	// re-arms from a due time that a frame may already have moved.
	EXPECT_EQ(desk.deliver(), 0U);
	EXPECT_EQ(desk.deliver(LIVE_FLIGHT_NS - 1), 0U);
	EXPECT_EQ(desk.session().pipe().in_flight(), 2U);
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID)
		<< "still the venue's touch: nothing of ours has landed on it";
}

TEST(AppLiveSessionLatency, TheClockReachingTheDueTimeDeliversIt) {
	live_desk desk(delayed());
	ASSERT_TRUE(desk.seed_touch());

	EXPECT_EQ(desk.deliver(LIVE_FLIGHT_NS), 2U);
	EXPECT_EQ(desk.session().pipe().in_flight(), 0U);
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID + 1)
		<< "the same quotes, one flight time later";
	EXPECT_EQ(desk.book().best_ask(), LIVE_TOUCH_ASK - 1);
	EXPECT_EQ(desk.report().wire_delivered, 2U);
	EXPECT_EQ(desk.report().orders_in_flight, 0U);
}

TEST(AppLiveSessionLatency, ADueTimeIsWhatATimerWouldArmFrom) {
	live_desk desk(delayed());
	const std::uint64_t started = desk.session().clock().now_ns();
	ASSERT_TRUE(desk.seed_touch());

	// serve.cpp sleeps until exactly this and then calls deliver_due. If it
	// were ever empty while something was in flight, the timer would fall back
	// to its idle tick and the modelled delay would become "a millisecond or
	// so".
	const auto due = desk.session().next_due_ns();
	ASSERT_TRUE(due.has_value());
	EXPECT_EQ(*due, started + LIVE_FLIGHT_NS);

	ASSERT_EQ(desk.deliver(LIVE_FLIGHT_NS), 2U);
	EXPECT_FALSE(desk.session().next_due_ns().has_value())
		<< "nothing left in flight, so nothing left to wake up for";
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID + 1);
}

// --- what the delay is actually for ---------------------------------------

TEST(AppLiveSessionLatency, AQuoteInFlightWhenTheMarketMovesIsPickedOff) {
	// Why a modelled delay is worth having at all, in one case: a quote decided
	// against one market is applied to a later one. Our 101/103 is written
	// while the venue shows 100/104, and by the time the wire hands it over the
	// venue is at 110/114 - so the 103 offer is well below the market and
	// trades the instant it lands. Adverse selection, which is the cost of
	// latency, and which a run with an instantaneous order path cannot see at
	// all.
	live_session_options options = delayed();
	// One quote for the run. Left at the default cadence the move below would
	// requote, and a requote withdraws both sides first - so the cancel would
	// retire the very order this case is about, and what landed would be a
	// quote about the *new* market rather than a stale one. Both frames are
	// stamped at market time 0, so any non-zero interval throttles the second;
	// the first is never throttled, since there is no previous quote to be too
	// close to.
	options.quoting.requote_interval_ns = 1;
	live_desk desk(options);
	ASSERT_TRUE(desk.seed_touch(LIVE_TOUCH_BID, LIVE_TOUCH_ASK));
	ASSERT_EQ(desk.session().pipe().in_flight(), 2U);

	// Both legs of the move in one diff, and it has to be both: a bid at 110
	// while the ask at 104 still stands leaves the replica locked, and a locked
	// replica is torn down as a gap rather than applied. Zero size is how a
	// diff spells "no level here". @see l2_book::set_level
	const auto moved_bids =
		std::to_array<book_level>({level(LIVE_TOUCH_BID, 0), level(110, 5)});
	const auto moved_asks =
		std::to_array<book_level>({level(LIVE_TOUCH_ASK, 0), level(114, 5)});
	ASSERT_EQ(desk.frame(diff(2, 0, moved_bids, moved_asks)),
			  market_data::sequence_action::apply);
	EXPECT_EQ(desk.book().best_ask(), 114U)
		<< "the venue's move is in the book at once - it is not ours to delay";

	ASSERT_GT(desk.deliver(LIVE_FLIGHT_NS), 0U);
	EXPECT_EQ(desk.book().volume_at_price(LIVE_TOUCH_BID + 1, side_t::bid), 1)
		<< "our stale bid rests nine ticks under a market that left it behind";
	EXPECT_EQ(desk.net(), -1) << "and our stale offer was taken: sold a lot "
								 "into a bid ten ticks above "
								 "where we were quoting";
	EXPECT_GT(desk.fills(), 0U);
}

TEST(AppLiveSessionLatency, AFrameDeliversWhatIsDueBeforeItQuotesAgain) {
	// on_event calls deliver_due first, so a busy market never waits for the
	// timer. The first frame's quotes are due by the time the second arrives,
	// and the second frame is what hands them over.
	live_desk desk(delayed());
	ASSERT_TRUE(desk.seed_touch());
	desk.advance(LIVE_FLIGHT_NS);
	ASSERT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID)
		<< "advancing the clock alone delivers nothing - somebody has to look";

	ASSERT_EQ(desk.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK),
			  market_data::sequence_action::apply);
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID + 1)
		<< "the frame looked, and the first frame's quotes went through";
	EXPECT_GT(desk.report().wire_delivered, 0U);
}

// --- back-pressure, which the wire refuses rather than drops --------------

// Back-pressure is not driven from here: a wire that fills up is relieved only
// by time passing, and `quote` waits for that by retrying - which a hand-driven
// clock never satisfies, so a case built here would hang rather than fail. The
// refusal path is pinned where it can be driven exactly, against a stub sink,
// in latency_pipe.test.cpp. @see AppLatencyPipe
} // namespace
