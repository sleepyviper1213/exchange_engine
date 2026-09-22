#include "order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

// What a trade says about itself once a book has printed it: which execution it
// is on this listing's tape, which way it went, when the aggressor arrived, and
// which lifecycle records belong to it.
//
// order_outcomes.test.cpp covers *which* records a book emits. This covers the
// identity stamped on them, which is what a downstream tape, a dedupe and a
// per-client execution report are all built out of. @see TODO.md #7

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;

namespace {

/// @brief The FILL records for @p id, which is where a trade id shows up on the
///        lifecycle side.
std::vector<order_outcome>
identity_fills_for(const std::vector<order_outcome> &all, order_id_t id) {
	std::vector<order_outcome> mine;
	for (const order_outcome &o : all)
		if (o.id == id && o.type == OutcomeType::FILL) mine.push_back(o);
	return mine;
}

/// @brief A receipt time that is obviously not a clock read taken during the
///        match - no test here runs in 2023.
constexpr std::uint64_t IDENTITY_RECEIVED_AT = 1'700'000'000'000'000'000ULL;

} // namespace

// --------------------------------------------------------------------------
// The tape
// --------------------------------------------------------------------------

TEST(OrderBookTradeIdentity, ExecutionsAreNumberedFromOneAndDensely) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	EXPECT_EQ(ob.last_trade_id(), 0U) << "resting alone prints nothing";

	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 4},
				   trades,
				   outcomes);
	ob.place_order({.id = 3, .side = side_t::bid, .price = 100, .qty = 4},
				   trades,
				   outcomes);

	ASSERT_EQ(trades.size(), 2U);
	EXPECT_EQ(trades[0].id, 1U);
	EXPECT_EQ(trades[1].id, 2U);
	// Dense, which is what lets a consumer tell a gap from a reordering.
	EXPECT_EQ(ob.last_trade_id(), 2U);
}

TEST(OrderBookTradeIdentity, ASweepNumbersEveryPrintItMakes) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::ask, .price = 101, .qty = 5},
				   trades,
				   outcomes);
	// One command, two levels, two prints - separate executions rather than one
	// aggregate, because they happened at different prices.
	ob.place_order({.id = 3, .side = side_t::bid, .price = 101, .qty = 10},
				   trades,
				   outcomes);

	ASSERT_EQ(trades.size(), 2U);
	EXPECT_EQ(trades[0].id, 1U);
	EXPECT_EQ(trades[1].id, 2U);
}

TEST(OrderBookTradeIdentity, ClearRestartsTheTape) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ASSERT_EQ(ob.last_trade_id(), 1U);

	// A session boundary, which is also where client order ids become reusable.
	ob.clear();
	EXPECT_EQ(ob.last_trade_id(), 0U);

	trades.clear();
	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ASSERT_EQ(trades.size(), 1U);
	EXPECT_EQ(trades[0].id, 1U);
}

TEST(OrderBookTradeIdentity, RestoringTheTapeResumesNumberingAfterIt) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	// What a recovery does before replaying: the executions a previous session
	// published are not in this book, but their numbers are spent.
	ob.restore_trade_id(41);
	EXPECT_EQ(ob.last_trade_id(), 41U);

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);

	ASSERT_EQ(trades.size(), 1U);
	EXPECT_EQ(trades[0].id, 42U);
}

// --------------------------------------------------------------------------
// Direction and time
// --------------------------------------------------------------------------

TEST(OrderBookTradeIdentity, TheSideRecordedIsTheOneThatTookLiquidity) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ASSERT_EQ(trades.size(), 1U);
	// A buy lifting an offer: an uptick, whoever was resting.
	EXPECT_EQ(trades[0].aggressor_side, side_t::bid);

	ob.place_order({.id = 3, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 4, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ASSERT_EQ(trades.size(), 2U);
	EXPECT_EQ(trades[1].aggressor_side, side_t::ask);
}

TEST(OrderBookTradeIdentity, TheTimestampIsTheAggressorsReceiptTime) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	// Two resting orders stamped at one time, an aggressor stamped at another.
	ob.place_order(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 5, .timestamp = 1},
		trades,
		outcomes);
	ob.place_order(
		{.id = 2, .side = side_t::ask, .price = 101, .qty = 5, .timestamp = 2},
		trades,
		outcomes);
	ob.place_order({.id        = 3,
					.side      = side_t::bid,
					.price     = 101,
					.qty       = 10,
					.timestamp = IDENTITY_RECEIVED_AT},
				   trades,
				   outcomes);

	ASSERT_EQ(trades.size(), 2U);
	// The aggressor's, not the maker's - and identical across both prints,
	// because one arrival produced both. A clock read at match time would have
	// given two different values, and neither would replay.
	EXPECT_EQ(trades[0].timestamp, IDENTITY_RECEIVED_AT);
	EXPECT_EQ(trades[1].timestamp, IDENTITY_RECEIVED_AT);
}

// --------------------------------------------------------------------------
// The join to the lifecycle stream
// --------------------------------------------------------------------------

TEST(OrderBookTradeIdentity, BothSidesOfAnExecutionNameTheSamePrint) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 4},
				   trades,
				   outcomes);

	ASSERT_EQ(trades.size(), 1U);
	const std::vector<order_outcome> maker = identity_fills_for(outcomes, 1);
	const std::vector<order_outcome> taker = identity_fills_for(outcomes, 2);
	ASSERT_EQ(maker.size(), 1U);
	ASSERT_EQ(taker.size(), 1U);

	// This is the whole of the per-side execution report: each side's own
	// remaining quantity, joined to one print that carries the price and the
	// direction. Without the join a client is told it filled and not at what.
	EXPECT_EQ(maker[0].trade_id, trades[0].id);
	EXPECT_EQ(taker[0].trade_id, trades[0].id);
	EXPECT_EQ(maker[0].remaining, 6);
	EXPECT_EQ(taker[0].remaining, 0);
}

TEST(OrderBookTradeIdentity, EachPrintOfASweepIsNamedByItsOwnFills) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 5},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::ask, .price = 101, .qty = 5},
				   trades,
				   outcomes);
	ob.place_order({.id = 3, .side = side_t::bid, .price = 101, .qty = 10},
				   trades,
				   outcomes);

	ASSERT_EQ(trades.size(), 2U);
	const std::vector<order_outcome> taker = identity_fills_for(outcomes, 3);
	ASSERT_EQ(taker.size(), 2U);
	// In print order, so an aggressor's two fills are told apart by which
	// execution each names rather than only by their cumulative totals.
	EXPECT_EQ(taker[0].trade_id, trades[0].id);
	EXPECT_EQ(taker[1].trade_id, trades[1].id);
	EXPECT_NE(taker[0].trade_id, taker[1].trade_id);
}

TEST(OrderBookTradeIdentity, ARecordThatReportsNoExecutionNamesNoPrint) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.cancel_order(1, outcomes);
	ob.cancel_order(99, outcomes);

	EXPECT_TRUE(trades.empty());
	ASSERT_EQ(outcomes.size(), 3U); // ACCEPTED, CANCELLED, CANCEL_REJECTED
	for (const order_outcome &record : outcomes)
		EXPECT_EQ(record.trade_id, 0U) << "on " << to_string(record.type);
}

// --------------------------------------------------------------------------
// What the book deliberately does not stamp
// --------------------------------------------------------------------------

TEST(OrderBookTradeIdentity, TheBookDoesNotSequenceWhatItEmits) {
	order_book ob;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);

	// A book has no notion of the command stream it sits in - that belongs to
	// execution::matching_engine, which stamps these on the way out. Zero here
	// is the contract rather than an omission: it is what keeps the book a pure
	// function of (command, book), and therefore replayable.
	ASSERT_EQ(trades.size(), 1U);
	EXPECT_EQ(trades[0].sequence, 0U);
	for (const order_outcome &record : outcomes) EXPECT_EQ(record.sequence, 0U);
}
