// The lane wired the way a deployment wires it: one router, a watched listing,
// an unwatched one, and the loop closing all the way round.
//
// The end-to-end claim is the one worth having, and it is the whole reason the
// post-trade lane trips a breaker rather than returning a verdict: a rule that
// fires on the dispatcher thread has to change what the *submit* path does, and
// nothing in `pre_trade/` was changed to make that work. Feed the monitor, poll
// it, and the gate in front of the strategy starts refusing new liquidity while
// still taking cancels.

#include "post_trade.fixture.hpp"
#include "event/event_dispatcher.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::risk;
using namespace exchange::risk::hooks;
using namespace exchange::risk::hooks::post_trade;

// A watched router is still what a dispatcher accepts, and a gate is still what
// it routes to. Asserted here rather than in the module for the reason
// feedback.test.cpp already gives: naming `event_handler` there would mean
// including the whole dispatcher for one line.
static_assert(event_handler<post_trade_router>);
static_assert(watched_gate<test_gate>,
			  "a gate has to be able to say what it thinks is working, or the "
			  "silence rule has nothing to be about");

/// @brief A gate that answers everything the return path needs and nothing the
///        post-trade lane does. It must still be routable.
struct unwatchable_gate {
	[[nodiscard]] symbol_id_t symbol() const noexcept { return SYMBOL; }

	void on_trades(std::span<const trade>) noexcept {}

	void on_outcomes(std::span<const order_outcome>) noexcept {}
};

static_assert(feedback_gate<unwatchable_gate>);
static_assert(!watched_gate<unwatchable_gate>,
			  "which is why the concept is split: a three-function double must "
			  "not have to grow a fourth for a lane it does not use");

TEST(PostTradeRouting, FeedsTheMonitorBesideTheGateThatScreensTheListing) {
	post_trade_desk desk{surveillance()};
	const std::array prints{filled_at(1, 2, 100, 5)};
	const std::array records{post_trade_ack(1)};

	EXPECT_EQ(desk.router().on_trades(SYMBOL, prints), 1U);
	EXPECT_EQ(desk.router().on_outcomes(SYMBOL, records), 1U);

	EXPECT_EQ(desk.monitor().fills().total_executions(), 1U);
	EXPECT_EQ(desk.monitor().ratio().total_messages(), 1U);
	EXPECT_EQ(desk.monitor().silence().outcomes(), 1U);
}

TEST(PostTradeRouting, AnotherListingsEventsReachTheMonitorNotAtAll) {
	post_trade_desk desk{surveillance()};
	const std::array prints{filled_at(1, 2, 900, 50)};

	EXPECT_EQ(desk.router().on_trades(OTHER_SYMBOL, prints), 1U);

	EXPECT_EQ(desk.router().applied_trades(), 1U) << "its gate was still fed";
	EXPECT_EQ(desk.monitor().fills().total_executions(), 0U)
		<< "a monitor fed another listing's prints would count executions the "
		   "account never had and mark a tape that is not its own";
	EXPECT_EQ(desk.monitor().fills().last_price(), 0U);
}

TEST(PostTradeRouting, AnUnroutedListingReachesNeitherHalf) {
	post_trade_desk desk{surveillance()};
	const std::array prints{filled_at(1, 2, 100, 5)};

	EXPECT_EQ(desk.router().on_trades(UNSCREENED_SYMBOL, prints), 1U)
		<< "consumed, because refusing it would stall the dispatcher";
	EXPECT_EQ(desk.router().unrouted(), 1U);
	EXPECT_EQ(desk.router().applied_trades(), 0U);
	EXPECT_EQ(desk.monitor().fills().total_executions(), 0U);
}

TEST(PostTradeRouting, KnowsWhichListingsAreWatched) {
	post_trade_desk desk{surveillance()};

	EXPECT_EQ(desk.router().listings(), 2U);
	EXPECT_EQ(desk.router().watched(), 1U);
	EXPECT_EQ(desk.router().monitor_for(SYMBOL), &desk.monitor());
	EXPECT_EQ(desk.router().monitor_for(OTHER_SYMBOL), nullptr)
		<< "a listing may have a gate and no monitor";
	EXPECT_EQ(desk.router().monitor_for(UNSCREENED_SYMBOL), nullptr);
}

TEST(PostTradeRouting, PollWithNothingWatchedIsFree) {
	// No monitor attached at all, which is the common deployment: poll() has to
	// be a compare and a return, and must not read a clock for nobody.
	position_book positions{post_trade_desk::LISTINGS};
	circuit_breaker breaker;
	manual_clock clock;
	recording_sink sink;
	test_gate gate{sink, SYMBOL, permissive(), positions, breaker, 0, clock};

	post_trade_router router{post_trade_desk::LISTINGS, clock};
	router.attach(gate);

	EXPECT_EQ(router.watched(), 0U);
	EXPECT_EQ(router.poll(), 0U);
}

TEST(PostTradeRouting, PollTripsOnlyWhenTheGateThinksItIsExposed) {
	post_trade_limits limits  = surveillance();
	limits.outcome_timeout_ns = POST_TRADE_TIMEOUT_NS;
	post_trade_desk desk{limits};

	desk.clock().set(POST_TRADE_TIMEOUT_NS * 4);
	EXPECT_EQ(desk.router().poll(), 0U)
		<< "silent for four timeouts, and nothing working - an idle strategy";
	EXPECT_EQ(desk.state(), trading_state::NORMAL);

	EXPECT_TRUE(desk.place(SYMBOL, 1, 100, 10));
	EXPECT_EQ(desk.gate(SYMBOL).working_orders(), 1U);

	desk.clock().advance(POST_TRADE_TIMEOUT_NS + 1);
	EXPECT_EQ(desk.router().poll(), 1U);
	EXPECT_EQ(desk.cause(), trip_cause::STALE_WORKING);
}

TEST(PostTradeRouting, APostTradeTripStopsThePreTradeScreen) {
	post_trade_limits limits  = surveillance();
	limits.outcome_timeout_ns = POST_TRADE_TIMEOUT_NS;
	post_trade_desk desk{limits};

	EXPECT_TRUE(desk.place(SYMBOL, 1, 100, 10));
	const auto passed_before = desk.gate(SYMBOL).passed();

	desk.clock().advance(POST_TRADE_TIMEOUT_NS + 1);
	ASSERT_EQ(desk.router().poll(), 1U);

	// The whole point of the lane: a rule that ran on the dispatcher thread
	// changes what the submit path does, through one byte the screen already
	// reads.
	EXPECT_TRUE(desk.place(SYMBOL, 2, 100, 10))
		<< "a risk refusal is not back-pressure - the batch was delivered";
	EXPECT_EQ(desk.gate(SYMBOL).passed(), passed_before)
		<< "but nothing new reached the sink";
	EXPECT_EQ(desk.gate(SYMBOL).breaches(breach::HALTED), 1U);

	EXPECT_TRUE(desk.gate(SYMBOL).submit(command::cancel(SYMBOL, 1)))
		<< "and the withdrawal a stalled strategy most needs still goes";
	EXPECT_EQ(desk.gate(SYMBOL).passed(), passed_before + 1);
}

TEST(PostTradeRouting, AnOutcomeThroughTheRouterKeepsTheWatchdogQuiet) {
	post_trade_limits limits  = surveillance();
	limits.outcome_timeout_ns = POST_TRADE_TIMEOUT_NS;
	post_trade_desk desk{limits};

	EXPECT_TRUE(desk.place(SYMBOL, 1, 100, 10));

	// An ACCEPTED changes nothing in the gate's ledger - the order is still
	// working - so this isolates the beat from the retirement.
	desk.clock().advance(POST_TRADE_TIMEOUT_NS);
	const std::array records{post_trade_ack(1)};
	desk.router().on_outcomes(SYMBOL, records);

	desk.clock().advance(POST_TRADE_TIMEOUT_NS);
	EXPECT_EQ(desk.gate(SYMBOL).working_orders(), 1U);
	EXPECT_EQ(desk.router().poll(), 0U)
		<< "the silence is measured from the last thing heard, and something "
		   "was heard";
	EXPECT_EQ(desk.state(), trading_state::NORMAL);
}

} // namespace
