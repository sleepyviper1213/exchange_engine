#include "strategy/backtest/quoter.hpp"

#include "backtest.fixture.hpp"
#include "strategy/backtest/session.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// The reference trader. It has no edge and is not meant to — what these cases
// pin is that it drives the harness the way a real quoter would: it improves on
// the touch rather than crossing it, it cancel-replaces rather than
// accumulating, and it believes the outcome stream about what it still has
// working.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy::backtest;

namespace {

/// @brief The quoter over a recording sink, so a case can read the commands it
///        wrote without an engine in the way.
struct quoter_under_test {
	symbol_spec spec = unit_listing();
	recording_sink sink;
	spread_quoter<recording_sink> quoter;
	market_data::l2_book replica;

	explicit quoter_under_test(quoter_options options = {})
		: quoter(sink, spec, options) {}

	void market(std::int64_t bid, std::int64_t ask, std::uint64_t now_ns = 0) {
		replica.clear();
		if (bid > 0) replica.set_level(side_t::bid, bid, 50);
		if (ask > 0) replica.set_level(side_t::ask, ask, 50);
		quoter.on_market(replica, now_ns);
	}

	[[nodiscard]] std::size_t count(command::Type type) const {
		std::size_t n = 0;
		for (const command &cmd : sink.commands())
			n += static_cast<std::size_t>(cmd.type == type);
		return n;
	}
};

} // namespace

TEST(BacktestQuoter, QuotesInsideTheVenuesTouchOnBothSides) {
	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());

	ASSERT_EQ(fixture.sink.size(), 2U);
	EXPECT_EQ(placed(fixture.sink.commands(), 0).side, side_t::bid);
	EXPECT_EQ(placed(fixture.sink.commands(), 0).price, 100U)
		<< "one tick better";
	EXPECT_EQ(placed(fixture.sink.commands(), 1).side, side_t::ask);
	EXPECT_EQ(placed(fixture.sink.commands(), 1).price, 101U);
}

TEST(BacktestQuoter, DoesNothingUntilTheVenueShowsBothSides) {
	quoter_under_test fixture;
	fixture.market(99, 0);
	EXPECT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.sink.size(), 0U);
	EXPECT_EQ(fixture.quoter.quotes(), 0U);
}

// A requote that changes nothing is a cancel and a place for no reason. It
// would also give the run a churn figure that says more about the quoter than
// about the market.
TEST(BacktestQuoter, LeavesAQuoteAloneWhileTheTouchHasNotMoved) {
	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	fixture.sink.clear();

	fixture.market(99, 102);
	EXPECT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.sink.size(), 0U);
	EXPECT_EQ(fixture.quoter.quotes(), 2U) << "still the original pair";
}

TEST(BacktestQuoter, CancelsAndReplacesWhenTheTouchMoves) {
	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t first_bid = fixture.quoter.live_order(side_t::bid);
	fixture.sink.clear();

	fixture.market(100, 103);
	ASSERT_TRUE(fixture.quoter.flush());

	EXPECT_EQ(fixture.count(command::Type::CANCEL), 2U);
	EXPECT_EQ(fixture.count(command::Type::PLACE), 2U);
	EXPECT_EQ(fixture.sink.commands()[0].as_cancel(), first_bid);
	EXPECT_NE(fixture.quoter.live_order(side_t::bid), first_bid)
		<< "a replacement is a different order and carries a different id";
	EXPECT_EQ(fixture.quoter.quoted_price(side_t::bid), 101U);
}

TEST(BacktestQuoter, WithdrawsBothSidesWhenTheVenueGoesOneSided) {
	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	fixture.sink.clear();

	fixture.market(99, 0);
	ASSERT_TRUE(fixture.quoter.flush());

	EXPECT_EQ(fixture.count(command::Type::CANCEL), 2U);
	EXPECT_EQ(fixture.quoter.live_order(side_t::bid), 0U);
	EXPECT_EQ(fixture.quoter.live_order(side_t::ask), 0U);
}

// Without the feedback the quoter would keep cancelling an id that no longer
// exists on every requote, and never put the side back.
TEST(BacktestQuoter, ForgetsAQuoteThatFilledInFull) {
	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t bid = fixture.quoter.live_order(side_t::bid);

	const order_outcome record = filled(bid, 1);
	fixture.quoter.on_outcomes({&record, 1});

	EXPECT_EQ(fixture.quoter.live_order(side_t::bid), 0U);
	EXPECT_NE(fixture.quoter.live_order(side_t::ask), 0U)
		<< "the ask is untouched";
}

TEST(BacktestQuoter, KeepsAQuoteThatOnlyPartlyFilled) {
	quoter_under_test fixture{quoter_options{.improve_ticks = 1, .lots = 10}};
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t bid = fixture.quoter.live_order(side_t::bid);

	const order_outcome record = partially_filled(bid, 10, 4);
	fixture.quoter.on_outcomes({&record, 1});

	EXPECT_EQ(fixture.quoter.live_order(side_t::bid), bid) << "six lots to go";
}

// Market time, from the feed — so the cadence is a property of the capture and
// not of how fast the machine replayed it.
TEST(BacktestQuoter, HoldsAQuoteForTheRequoteInterval) {
	quoter_under_test fixture{quoter_options{.requote_interval_ns = 1000}};
	fixture.market(99, 102, 10000);
	ASSERT_TRUE(fixture.quoter.flush());
	fixture.sink.clear();

	fixture.market(100, 103, 10500); // 500 ns later: too soon
	EXPECT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.sink.size(), 0U);

	fixture.market(100, 103, 11000); // now the interval has passed
	ASSERT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.count(command::Type::PLACE), 2U);
}

TEST(BacktestQuoter, RefusesToQuoteATouchThatIsNotOnTheTickGrid) {
	const symbol_spec coarse{0, "TEST", 0, 0, 10, 1, 100}; // tick of 10
	recording_sink sink;
	spread_quoter<recording_sink> quoter(sink, coarse);
	market_data::l2_book replica;
	replica.set_level(side_t::bid, 99, 50); // not a multiple of 10
	replica.set_level(side_t::ask, 120, 50);

	quoter.on_market(replica, 0);
	EXPECT_TRUE(quoter.flush());
	EXPECT_EQ(sink.size(), 0U);
	EXPECT_EQ(quoter.off_grid(), 1U);
}

TEST(BacktestQuoter, KeepsTheBatchWhenTheSinkRefuses) {
	quoter_under_test fixture;
	fixture.sink.refuse(true);
	fixture.market(99, 102);

	EXPECT_FALSE(fixture.quoter.flush());
	EXPECT_EQ(fixture.quoter.stalls(), 1U);
	EXPECT_EQ(fixture.quoter.submitted(), 0U);

	fixture.sink.refuse(false);
	EXPECT_TRUE(fixture.quoter.flush()) << "the batch survived the refusal";
	EXPECT_EQ(fixture.sink.size(), 2U);
}

// The integration the CLI actually runs: the quoter against the whole harness,
// with the market moving through the quote it left inside the spread.
TEST(BacktestQuoter, TradesAgainstARecordingWhenDrivenByASession) {
	const symbol_spec spec = unit_listing();
	session run(spec);
	spread_quoter<session::gate_type> quoter(
		run.sink(),
		spec,
		quoter_options{.improve_ticks = 1, .lots = 4});
	static_assert(market_observer<decltype(quoter)>,
				  "the session must see the quoter's market hook, or it would "
				  "quote once and never again");

	ASSERT_TRUE(
		run.on_snapshot(seed(10, {level(99, 50)}, {level(102, 50)}), quoter));
	run.on_event(diff(11, 1000, {}, {}), quoter);
	ASSERT_EQ(quoter.quoted_price(side_t::bid), 100U) << "one inside 99";
	ASSERT_EQ(run.book().volume_at_price(100, side_t::bid), 4);

	// The whole market steps down to 97 / 99, taking the offer through the bid
	// the quoter left at 100. The new spread is two ticks, so there is no room
	// inside it and the quoter holds the quote it already has rather than
	// pulling it out of the way — which is what leaves something there to fill.
	run.on_event(diff(12,
					  2000,
					  {level(99, 0), level(97, 50)},
					  {level(102, 0), level(99, 3)}),
				 quoter);
	run.finish(quoter);

	const report &result = run.result();
	EXPECT_EQ(result.passive_fills, 1U);
	EXPECT_EQ(result.passive_lots, 3) << "bounded by what the venue offered";
	EXPECT_EQ(result.aggressive_fills, 0U)
		<< "a quoter that improves on the touch never crosses it";
	EXPECT_EQ(result.net_lots, 3);
	EXPECT_EQ(result.misroutes, 0U);
	EXPECT_EQ(result.commands_dropped, 0U);
	EXPECT_NE(quoter.live_order(side_t::bid), 0U)
		<< "one of the four lots filled, so the quote is still working";
}
