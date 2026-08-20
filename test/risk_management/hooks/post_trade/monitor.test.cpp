// The lane's composer: which rule each half of the event stream reaches, and
// what a monitor nobody configured costs.
//
// The three rules have their own suites, so nothing here re-asserts their
// arithmetic. What is only checkable here is the wiring: that a print feeds two
// rules and not one, that every outcome beats the watchdog while only some
// count as messages, and that a monitor with every threshold off is inert
// however hard it is fed - which is what a deployment that has not thought
// about surveillance gets, and must not be a surprise.

#include "post_trade.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::risk;
using namespace exchange::risk::hooks::post_trade;

TEST(PostTradeMonitor, APrintFeedsTheBurstCountersAndTheRatiosDenominator) {
	post_trade_watch watch{surveillance()};
	const std::array prints{print(100, 3), print(101, 4)};

	EXPECT_FALSE(watch.monitor().on_trades(prints, 0));

	EXPECT_EQ(watch.monitor().fills().total_executions(), 2U);
	EXPECT_EQ(watch.monitor().fills().total_volume(), 7U);
	EXPECT_EQ(watch.monitor().ratio().total_executions(), 2U)
		<< "an execution is what buys a strategy its messages";
	EXPECT_EQ(watch.monitor().ratio().total_messages(), 0U)
		<< "and a print is not one";
}

TEST(PostTradeMonitor, EveryOutcomeBeatsTheWatchdog) {
	post_trade_watch watch{surveillance()};
	const std::array records{post_trade_ack(1),
							 filled(1, 10),
							 post_trade_ioc_drop(2, 5)};

	EXPECT_FALSE(watch.monitor().on_outcomes(records, 500));

	EXPECT_EQ(watch.monitor().silence().outcomes(), 3U)
		<< "a fill and an IOC drop are still evidence the path is alive";
	EXPECT_EQ(watch.monitor().silence().last_outcome_ns(), 500U);
	EXPECT_EQ(watch.monitor().ratio().total_messages(), 1U)
		<< "but only the ack is a message the venue had to process";
}

TEST(PostTradeMonitor, TheRatioSeesTheWholeBatchsMessages) {
	post_trade_limits limits          = surveillance();
	limits.max_messages_per_execution = 1;
	limits.min_messages_to_judge      = 2;
	post_trade_watch watch{limits};

	const std::array records{post_trade_ack(1), post_trade_ack(2)};
	EXPECT_TRUE(watch.monitor().on_outcomes(records, 0))
		<< "two orders and nothing traded is a ratio nothing satisfies";
	EXPECT_EQ(watch.cause(), trip_cause::ORDER_TRADE_RATIO);
}

TEST(PostTradeMonitor, TripsAggregateAcrossTheThreeRules) {
	post_trade_limits limits         = surveillance();
	limits.max_executions_per_window = 1;
	limits.outcome_timeout_ns        = POST_TRADE_TIMEOUT_NS;
	post_trade_watch watch{limits};

	const std::array prints{print(100, 1), print(100, 1)};
	EXPECT_TRUE(watch.monitor().on_trades(prints, 0));
	EXPECT_EQ(watch.cause(), trip_cause::FILL_BURST);
	EXPECT_EQ(watch.monitor().trips(), 1U);

	// The silence rule is independent of the burst one, and an operator who
	// re-arms has to be able to see it fire on its own.
	watch.breaker().arm();
	EXPECT_TRUE(watch.monitor().poll(POST_TRADE_TIMEOUT_NS * 2, 1));
	EXPECT_EQ(watch.cause(), trip_cause::STALE_WORKING);
	EXPECT_EQ(watch.monitor().trips(), 2U)
		<< "the aggregate is the number to look at before choosing which rule "
		   "to read";
}

TEST(PostTradeMonitor, PollIsTheOnlyWayTheSilenceRuleFires) {
	post_trade_limits limits  = surveillance();
	limits.outcome_timeout_ns = POST_TRADE_TIMEOUT_NS;
	post_trade_watch watch{limits};

	const std::array prints{print(100, 1)};
	const std::uint64_t past = POST_TRADE_TIMEOUT_NS * 2;

	EXPECT_FALSE(watch.monitor().on_trades(prints, past))
		<< "an arriving event cannot notice an absence";
	EXPECT_FALSE(watch.is_open());

	EXPECT_TRUE(watch.monitor().poll(past, 1));
}

TEST(PostTradeMonitor, ADisabledMonitorIsInertHoweverHardItIsFed) {
	post_trade_watch watch{surveillance()};

	for (price_t price = 100; price < 300; ++price) {
		const std::array prints{print(price, 1000)};
		const std::array records{post_trade_ack(price)};
		watch.monitor().on_trades(prints, 0);
		watch.monitor().on_outcomes(records, 0);
	}
	EXPECT_FALSE(watch.monitor().poll(POST_TRADE_TIMEOUT_NS * 1000, 1000));

	EXPECT_FALSE(watch.is_open())
		<< "a rule nobody sized is a rule that trips at the wrong time";
	EXPECT_EQ(watch.monitor().trips(), 0U);
	EXPECT_EQ(watch.monitor().fills().total_executions(), 200U)
		<< "off means it does not act, not that it stops counting";
}

TEST(PostTradeMonitor, ReadsBackWhatItWasBuiltWith) {
	post_trade_limits limits          = surveillance();
	limits.max_adverse_run            = 7;
	limits.max_messages_per_execution = 250;
	post_trade_watch watch{limits, 900};

	EXPECT_EQ(watch.monitor().symbol(), SYMBOL);
	EXPECT_EQ(watch.monitor().limits().max_adverse_run, 7U);
	EXPECT_TRUE(watch.monitor().limits().has_ratio_limit());
	EXPECT_FALSE(watch.monitor().limits().has_burst_limit());
	EXPECT_EQ(watch.monitor().ratio().threshold(), 250U);
	EXPECT_EQ(watch.monitor().silence().last_outcome_ns(), 900U)
		<< "the silence rule measures from construction until it is fed";
	EXPECT_EQ(watch.monitor().fills().window_ns(), POST_TRADE_WINDOW_NS);
}

TEST(PostTradeMonitor, AnEmptySpanChangesNothing) {
	post_trade_limits limits          = surveillance();
	limits.max_executions_per_window  = 1;
	limits.max_messages_per_execution = 1;
	limits.min_messages_to_judge      = 1;
	post_trade_watch watch{limits};

	EXPECT_FALSE(watch.monitor().on_trades({}, 0));
	EXPECT_FALSE(watch.monitor().on_outcomes({}, 0));
	EXPECT_FALSE(watch.is_open());
	EXPECT_EQ(watch.monitor().silence().outcomes(), 0U);
}

} // namespace
