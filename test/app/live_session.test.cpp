// The live topology, wired: does depth reach the book, does the gate see every
// command, does a fill come all the way back round.
//
// These are wiring cases, and the reason they are worth having is that every
// component here already has a suite of its own. What none of those can check
// is that the composition connects them in the order the design claims - that
// the gate is genuinely in front of the partition rather than beside it, that
// the quoter learns about its own fills, that a gap withdraws the liquidity it
// seeded. Each of those is one wrong line in a constructor away from being
// silently false.

#include "live_session.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::risk::hooks;
using namespace exchange::risk::hooks::system;

// The conformance that matters here - that a session is what run_live_feed can
// drive - is asserted in serve.cpp instead. Naming `live_handler` means
// including live_feed.hpp, which is Boost.Asio and a WebSocket client, for one
// line; `order_test` links neither and should not start.

// --- the depth half ------------------------------------------------------

TEST(AppLiveSession, SeedsTheEngineBookFromTheVenuesDepth) {
	live_desk desk;
	EXPECT_TRUE(desk.seed_touch(LIVE_TOUCH_BID, LIVE_TIGHT_ASK));

	ASSERT_EQ(desk.session().quoter().quotes(), 0U)
		<< "nothing of ours is resting, so the book's touch is the venue's";
	EXPECT_GT(desk.report().depth_commands, 0U);
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID);
	EXPECT_EQ(desk.book().best_ask(), LIVE_TIGHT_ASK)
		<< "the venue's published depth is resting in this process's own book";
	EXPECT_TRUE(desk.session().is_alive());
}

TEST(AppLiveSession, TheGateScreensTheDepthItSeeds) {
	// One lot per order, against a venue publishing five.
	live_session_options options;
	options.limits.max_order_qty = 1;
	live_desk desk{options};

	EXPECT_TRUE(desk.seed_touch(LIVE_TOUCH_BID, LIVE_TIGHT_ASK));

	EXPECT_GT(desk.session().gate().breaches(breach::ORDER_QUANTITY), 0U)
		<< "the gate is in front of the partition, not beside it";
	EXPECT_FALSE(desk.book().best_bid().has_value())
		<< "and what it refused never reached the book";
}

TEST(AppLiveSession, AGapWithdrawsTheLiquidityItSeeded) {
	live_desk desk;
	ASSERT_TRUE(desk.seed_touch(LIVE_TOUCH_BID, LIVE_TIGHT_ASK));
	ASSERT_TRUE(desk.book().best_bid().has_value());

	// Sequence 5 when 2 was expected: the replica cannot bridge the hole, so
	// the depth it seeded is no longer evidence about the venue.
	EXPECT_EQ(desk.move_touch(5, 101, 103), market_data::sequence_action::gap);

	EXPECT_EQ(desk.report().gaps, 1U);
	EXPECT_FALSE(desk.session().is_alive());
	EXPECT_FALSE(desk.book().best_bid().has_value())
		<< "matching against a dead replica is matching against the past";
}

TEST(AppLiveSession, AReconnectWithdrawsTheLiquidityItSeeded) {
	live_desk desk;
	ASSERT_TRUE(desk.seed_touch(LIVE_TOUCH_BID, LIVE_TIGHT_ASK));
	ASSERT_TRUE(desk.book().best_ask().has_value());

	desk.reconnect();

	EXPECT_EQ(desk.report().invalidations, 1U);
	EXPECT_FALSE(desk.book().best_ask().has_value());
	EXPECT_TRUE(desk.session().needs_snapshot())
		<< "and it asks for the snapshot that would make it live again";
}

// --- the passive quoter --------------------------------------------------

TEST(AppLiveSession, ThePassiveQuoterRestsInsideTheVenuesTouch) {
	live_desk desk;
	ASSERT_TRUE(desk.seed_touch());

	// 100 / 104 with one tick of improvement on each side.
	EXPECT_EQ(desk.session().quoter().quotes(), 2U);
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID + 1);
	EXPECT_EQ(desk.book().best_ask(), LIVE_TOUCH_ASK - 1);
	EXPECT_EQ(desk.session().gate().working_orders(), 2U)
		<< "and the gate is holding both of them against the position limit";
}

TEST(AppLiveSession, APassiveQuoteNeverTrades) {
	live_desk desk;
	ASSERT_TRUE(desk.seed_touch());
	ASSERT_EQ(desk.session().quoter().quotes(), 2U);

	// Four frames of a market walking one way through the resting quotes.
	for (market_data::sequence_t at = 2; at <= 5; ++at)
		desk.move_touch(at,
						LIVE_TOUCH_BID - static_cast<std::int64_t>(at),
						LIVE_TOUCH_ASK - static_cast<std::int64_t>(at));

	EXPECT_EQ(desk.fills(), 0U)
		<< "the liquidity a bridge seeds is rested with add_order, which does "
		   "not match - so a resting quote has nothing to fill against and "
		   "quoter_options::take_liquidity exists";
	EXPECT_EQ(desk.net(), 0);
}

// --- the taker, which is what a live run has to be ------------------------

TEST(AppLiveSession, TakingLiquidityTradesAgainstTheSeededDepth) {
	live_session_options options;
	options.quoting.take_liquidity = true;
	options.quoting.lots           = 2;
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());

	EXPECT_EQ(desk.session().quoter().takes(), 1U) << "one side per requote";
	EXPECT_GT(desk.fills(), 0U)
		<< "and it crossed real depth in a real book - the whole loop, from a "
		   "frame to a fill the post-trade monitor counted";
	EXPECT_EQ(desk.net(), 2) << "bought first, so long the size it took";
}

TEST(AppLiveSession, TakingAlternatesSidesSoThePositionWalksAboutFlat) {
	live_session_options options;
	options.quoting.take_liquidity = true;
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());
	ASSERT_EQ(desk.net(), 1);

	desk.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);
	EXPECT_EQ(desk.session().quoter().takes(), 2U);
	EXPECT_EQ(desk.net(), 0) << "the second take sold what the first bought";
}

TEST(AppLiveSession, ATakerLeavesNothingResting) {
	live_session_options options;
	options.quoting.take_liquidity = true;
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());

	EXPECT_EQ(desk.session().gate().working_orders(), 0U)
		<< "an IOC either fills or is dropped, so nothing of ours can be left "
		   "for the venue's next diff to cross";
	EXPECT_EQ(desk.book().best_bid(), LIVE_TOUCH_BID)
		<< "and the book's touch is the venue's, not ours";
}

TEST(AppLiveSession, TheGateRefusesATakeThatWouldBreachThePositionLimit) {
	// A limit of zero admits no position at all, so the projection refuses the
	// very first take. Alternating sides means the position never walks past
	// one lot on its own, which is the point of alternating - so the way to
	// show the limit binding is to set one it cannot satisfy.
	live_session_options options;
	options.quoting.take_liquidity   = true;
	options.limits.max_position_lots = 0;
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());

	EXPECT_GT(desk.session().gate().breaches(breach::POSITION_LIMIT), 0U);
	EXPECT_EQ(desk.net(), 0) << "nothing reached the book to move it";
	EXPECT_EQ(desk.fills(), 0U)
		<< "and the position the gate projects against is the one the fills "
		   "move, which is the loop this composition exists to close";
}

// --- the system lane -----------------------------------------------------

TEST(AppLiveSession, TheFeedWatchdogTripsWhenFramesStop) {
	live_session_options options;
	options.feed_timeout_ns = 1'000'000; // 1 ms of silence is a dead feed here
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());
	ASSERT_EQ(desk.session().breaker().state(), trading_state::NORMAL);

	desk.advance(options.feed_timeout_ns + 1);

	EXPECT_EQ(desk.session().breaker().state(), trading_state::CANCEL_ONLY)
		<< "a process that can no longer see the market must stop adding risk";
	EXPECT_EQ(desk.session().breaker().cause(), trip_cause::STALE_FEED);
	EXPECT_EQ(desk.session().feed_watchdog().trips(), 1U);
}

TEST(AppLiveSession, AFrameFeedsTheWatchdogRatherThanTrippingIt) {
	live_session_options options;
	options.feed_timeout_ns = 1'000'000;
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());

	// Just under the timeout, then a frame, then just under it again.
	desk.advance(options.feed_timeout_ns - 1);
	desk.session().clock().advance(2);
	desk.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);
	desk.advance(options.feed_timeout_ns - 1);

	EXPECT_EQ(desk.session().breaker().state(), trading_state::NORMAL)
		<< "the silence is measured from the last frame, not from the start";
	EXPECT_EQ(desk.session().feed_watchdog().trips(), 0U);
}

TEST(AppLiveSession, ADisabledWatchdogNeverTripsHoweverQuietItGets) {
	live_desk desk;               // feed_timeout_ns defaults to zero
	ASSERT_TRUE(desk.seed_touch());

	desk.advance(60'000'000'000); // a minute of silence

	EXPECT_EQ(desk.session().breaker().state(), trading_state::NORMAL);
	EXPECT_EQ(desk.session().feed_watchdog().trips(), 0U);
}

// --- the post-trade lane -------------------------------------------------

TEST(AppLiveSession, ThePostTradeMonitorSeesTheListingsExecutions) {
	live_session_options options;
	options.quoting.take_liquidity = true;
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());

	EXPECT_GT(desk.session().monitor().fills().total_executions(), 0U);
	EXPECT_GT(desk.session().monitor().ratio().total_messages(), 0U)
		<< "the outcome stream reaches it too, or its ratio has no numerator";
	EXPECT_GT(desk.session().monitor().silence().outcomes(), 0U)
		<< "and every outcome is evidence the return path is alive";
}

TEST(AppLiveSession, APostTradeTripStopsTheNextTake) {
	// One execution per burst window is all this listing may print.
	live_session_options options;
	options.quoting.take_liquidity                 = true;
	options.surveillance.max_executions_per_window = 1;
	live_desk desk{options};

	ASSERT_TRUE(desk.seed_touch());
	desk.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);

	EXPECT_EQ(desk.session().breaker().cause(), trip_cause::FILL_BURST);
	const auto before = desk.session().quoter().takes();

	desk.move_touch(3, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);

	EXPECT_GT(desk.session().quoter().takes(), before)
		<< "the quoter goes on writing - it is not the thing that stopped";
	EXPECT_GT(desk.session().gate().breaches(breach::HALTED), 0U)
		<< "but the gate refuses new liquidity, which is where a post-trade "
		   "trip becomes a pre-trade decision";
}

} // namespace
