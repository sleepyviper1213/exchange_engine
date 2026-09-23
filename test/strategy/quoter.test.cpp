#include "strategy/quoter.hpp"

#include "backtest/backtest.fixture.hpp"
#include "strategy/backtest/session.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

// The reference trader. It has no edge and is not meant to - what these cases
// pin is that it drives a host the way a real quoter would: it improves on the
// touch rather than crossing it, it amends its quote rather than accumulating,
// and it believes the outcome stream about what it still has working.
//
// The last case reaches for the backtest harness, and deliberately: the cheap
// cases here drive a recording sink, but "does it actually trade" needs a real
// matching engine on the other side of the gate, and `backtest::session` is the
// composition that assembles one in a single thread. `serve` assembles the same
// pieces across two, which is why the wiring there has a suite of its own.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy;
using namespace exchange::strategy::backtest;
using exchange::market_data::book_level;

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

	[[nodiscard]] std::size_t count(event::command_type type) const {
		std::size_t n = 0;
		for (const command &cmd : sink.commands())
			n += static_cast<std::size_t>(cmd.type == type);
		return n;
	}
};

} // namespace

TEST(StrategyQuoter, QuotesInsideTheVenuesTouchOnBothSides) {
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

TEST(StrategyQuoter, DoesNothingUntilTheVenueShowsBothSides) {
	quoter_under_test fixture;
	fixture.market(99, 0);
	EXPECT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.sink.size(), 0U);
	EXPECT_EQ(fixture.quoter.quotes(), 0U);
}

// A requote that changes nothing is a message for no reason. It would also give
// the run a churn figure that says more about the quoter than about the market.
TEST(StrategyQuoter, LeavesAQuoteAloneWhileTheTouchHasNotMoved) {
	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	fixture.sink.clear();

	fixture.market(99, 102);
	EXPECT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.sink.size(), 0U);
	EXPECT_EQ(fixture.quoter.quotes(), 2U) << "still the original pair";
}

// One command per side where a cancel-replace was two, and the order keeps its
// id - so a requote no longer burns a client order id per side per event and
// the venue's record of the quote is one lifecycle rather than a chain of them.
//
// The venue widens rather than steps, so neither of our quotes is in the other's
// way and the two amendments go out in side order. Which side moves first when
// one *is* in the way is MovesTheSideInTheWayFirst's subject, and it is not an
// implementation detail - it is what replaced withdrawing both sides up front.
TEST(StrategyQuoter, AmendsBothQuotesWhenTheTouchMoves) {
	using enum event::command_type;
	quoter_under_test fixture;
	fixture.market(99, 102); // quoting 100 / 101
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t first_bid = fixture.quoter.live_order(side_t::bid);
	const order_id_t first_ask = fixture.quoter.live_order(side_t::ask);
	fixture.sink.clear();

	fixture.market(98, 103); // now quoting 99 / 102
	ASSERT_TRUE(fixture.quoter.flush());

	EXPECT_EQ(fixture.count(MODIFY), 2U);
	EXPECT_EQ(fixture.count(CANCEL), 0U);
	EXPECT_EQ(fixture.count(PLACE), 0U);
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().id, first_bid);
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().price, 99U);
	EXPECT_EQ(fixture.sink.commands()[1].as_modify().id, first_ask);
	EXPECT_EQ(fixture.sink.commands()[1].as_modify().price, 102U);
	EXPECT_EQ(fixture.quoter.live_order(side_t::bid), first_bid)
		<< "an amendment leaves the same order in place";
	EXPECT_EQ(fixture.quoter.quoted_price(side_t::bid), 99U);
}

// The market moved up past our own offer, so amending the bid first would put
// it through a quote we still have resting - and this engine has no self-trade
// prevention (TODO.md #10). At most one side can be in the way, and it is the
// one that moves first.
TEST(StrategyQuoter, MovesTheSideInTheWayFirst) {
	quoter_under_test fixture;
	fixture.market(99, 102); // quoting 100 / 101
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t ask = fixture.quoter.live_order(side_t::ask);
	fixture.sink.clear();

	// The venue jumps to 105 / 108, so the new bid at 106 is through the offer
	// still resting at 101.
	fixture.market(105, 108);
	ASSERT_TRUE(fixture.quoter.flush());

	ASSERT_EQ(fixture.sink.size(), 2U);
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().id, ask)
		<< "the offer is amended out of the way before the bid is raised";
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().price, 107U);
	EXPECT_EQ(fixture.sink.commands()[1].as_modify().price, 106U);
}

// The other direction, which must not be reordered: amending the bid down
// first is what opens the spread for the offer that follows it.
TEST(StrategyQuoter, MovesTheBidFirstWhenTheMarketFallsAway) {
	quoter_under_test fixture;
	fixture.market(99, 102); // quoting 100 / 101
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t bid = fixture.quoter.live_order(side_t::bid);
	fixture.sink.clear();

	fixture.market(90, 93);
	ASSERT_TRUE(fixture.quoter.flush());

	ASSERT_EQ(fixture.sink.size(), 2U);
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().id, bid);
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().price, 91U);
	EXPECT_EQ(fixture.sink.commands()[1].as_modify().price, 92U);
}

// An amendment names the order's quantity, not its remainder, so restoring a
// partially filled quote to its full showing size means asking for what it
// traded plus the size it should show. Asking for `lots` flat would quietly
// shrink the quote, where cancel-and-replace used to put a full lot count back.
TEST(StrategyQuoter, AmendsAPartiallyFilledQuoteBackToItsFullSize) {
	quoter_under_test fixture{quoter_options{.improve_ticks = 1, .lots = 10}};
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t bid = fixture.quoter.live_order(side_t::bid);

	const order_outcome record = partially_filled(bid, 10, 4);
	fixture.quoter.on_outcomes({&record, 1});
	fixture.sink.clear();

	// Widening, so the bid is not amended through our own offer and goes first.
	fixture.market(98, 103);
	ASSERT_TRUE(fixture.quoter.flush());

	ASSERT_FALSE(fixture.sink.commands().empty());
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().id, bid);
	EXPECT_EQ(fixture.sink.commands()[0].as_modify().quantity, 14)
		<< "four traded plus ten to show";
}

// An amendment the venue would not apply leaves the quote resting where it was,
// while this quoter has already written down the price it asked for. Forgetting
// the order is the conservative repair - the next requote places a fresh one
// rather than amending against a price that is not there.
TEST(StrategyQuoter, ForgetsAQuoteWhoseAmendmentWasDeclined) {
	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t bid = fixture.quoter.live_order(side_t::bid);

	const order_outcome declined = order_outcome::modify_rejected(
		bid, reject_reason::UNKNOWN_ORDER);
	fixture.quoter.on_outcomes({&declined, 1});

	EXPECT_EQ(fixture.quoter.live_order(side_t::bid), 0U);
	EXPECT_NE(fixture.quoter.live_order(side_t::ask), 0U)
		<< "the ask is untouched";
}

TEST(StrategyQuoter, WithdrawsBothSidesWhenTheVenueGoesOneSided) {
	using enum event::command_type;

	quoter_under_test fixture;
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	fixture.sink.clear();

	fixture.market(99, 0);
	ASSERT_TRUE(fixture.quoter.flush());

	EXPECT_EQ(fixture.count(CANCEL), 2U);
	EXPECT_EQ(fixture.quoter.live_order(side_t::bid), 0U);
	EXPECT_EQ(fixture.quoter.live_order(side_t::ask), 0U);
}

// Without the feedback the quoter would keep cancelling an id that no longer
// exists on every requote, and never put the side back.
TEST(StrategyQuoter, ForgetsAQuoteThatFilledInFull) {
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

TEST(StrategyQuoter, KeepsAQuoteThatOnlyPartlyFilled) {
	quoter_under_test fixture{quoter_options{.improve_ticks = 1, .lots = 10}};
	fixture.market(99, 102);
	ASSERT_TRUE(fixture.quoter.flush());
	const order_id_t bid = fixture.quoter.live_order(side_t::bid);

	const order_outcome record = partially_filled(bid, 10, 4);
	fixture.quoter.on_outcomes({&record, 1});

	EXPECT_EQ(fixture.quoter.live_order(side_t::bid), bid) << "six lots to go";
}

// Market time, from the feed - so the cadence is a property of the capture and
// not of how fast the machine replayed it.
TEST(StrategyQuoter, HoldsAQuoteForTheRequoteInterval) {
	using enum event::command_type;

	quoter_under_test fixture{quoter_options{.requote_interval_ns = 1000}};
	fixture.market(99, 102, 10000);
	ASSERT_TRUE(fixture.quoter.flush());
	fixture.sink.clear();

	fixture.market(100, 103, 10500); // 500 ns later: too soon
	EXPECT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.sink.size(), 0U);

	fixture.market(100, 103, 11000); // now the interval has passed
	ASSERT_TRUE(fixture.quoter.flush());
	EXPECT_EQ(fixture.count(MODIFY), 2U);
}

TEST(StrategyQuoter, RefusesToQuoteATouchThatIsNotOnTheTickGrid) {
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

TEST(StrategyQuoter, KeepsTheBatchWhenTheSinkRefuses) {
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
TEST(StrategyQuoter, TradesAgainstARecordingWhenDrivenByASession) {
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
		run.on_snapshot(seed(10,
							 std::to_array<book_level>({level(99, 50)}),
							 std::to_array<book_level>({level(102, 50)})),
						quoter));
	run.on_event(diff(11, 1000, {}, {}), quoter);
	ASSERT_EQ(quoter.quoted_price(side_t::bid), 100U) << "one inside 99";
	ASSERT_EQ(run.book().volume_at_price(100, side_t::bid), 4);

	// The whole market steps down to 97 / 99, taking the offer through the bid
	// the quoter left at 100. The new spread is two ticks, so there is no room
	// inside it and the quoter holds the quote it already has rather than
	// pulling it out of the way - which is what leaves something there to fill.
	run.on_event(diff(12,
					  2000,
					  std::to_array<book_level>({level(99, 0), level(97, 50)}),
					  std::to_array<book_level>({level(102, 0), level(99, 3)})),
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
