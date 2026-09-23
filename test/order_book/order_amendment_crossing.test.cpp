#include "order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

// An amendment that moves an order's price is the one that can trade. Moving
// an order onto a crossing price without matching it would leave the book
// crossed, which is the one state every matching invariant assumes away - so a
// reprice re-crosses, and these cases pin what that produces.
//
// The priority rules themselves are order_amendment.test.cpp.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::orders;

namespace {

constexpr price_t CROSS_BID    = 100;
constexpr price_t CROSS_ASK    = 105;
constexpr timestamp_t CROSS_AT = 1'700'000'000'000'000'000ULL;

/// @brief A book with one resting bid at 100 and one resting ask at 105.
void cross_seed(order_book &book, quantity_t bid_qty, quantity_t ask_qty) {
	ASSERT_TRUE(book.place_order({.id    = 1,
								  .side  = side_t::bid,
								  .price = CROSS_BID,
								  .qty   = bid_qty})
					.empty());
	ASSERT_TRUE(book.place_order({.id    = 2,
								  .side  = side_t::ask,
								  .price = CROSS_ASK,
								  .qty   = ask_qty})
					.empty());
}

/// @brief The outcomes naming @p id, in the order the book emitted them.
std::vector<order_outcome> cross_for(const std::vector<order_outcome> &all,
									 order_id_t id) {
	std::vector<order_outcome> mine;
	for (const order_outcome &record : all)
		if (record.id == id) mine.push_back(record);
	return mine;
}

} // namespace

// Announced before the fills it causes, exactly as an ACCEPTED is: the
// amendment is what made the crossing happen, so it is reported first.
TEST(OrderAmendmentCrossing, ARepriceThroughTheSpreadTradesAtTheRestingPrice) {
	order_book book;
	cross_seed(book, 10, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order(
		{.id = 1, .price = CROSS_ASK, .quantity = 10, .timestamp = CROSS_AT},
		trades,
		outcomes);

	ASSERT_EQ(trades.size(), 1U);
	EXPECT_EQ(trades[0].aggressor, 1U);
	EXPECT_EQ(trades[0].resting, 2U);
	EXPECT_EQ(trades[0].price, CROSS_ASK)
		<< "trades print at the maker's price";
	EXPECT_EQ(trades[0].volume, 10);
	EXPECT_EQ(trades[0].aggressor_side, side_t::bid);
	EXPECT_EQ(trades[0].timestamp, CROSS_AT)
		<< "the amendment's receipt time, carried like an order's";

	const std::vector<order_outcome> mine = cross_for(outcomes, 1);
	ASSERT_EQ(mine.size(), 2U);
	EXPECT_EQ(mine[0].type, OutcomeType::MODIFIED);
	EXPECT_EQ(mine[0].remaining, 10);
	EXPECT_EQ(mine[1].type, OutcomeType::FILL);
	EXPECT_EQ(mine[1].status, OrderStatus::FILLED);
	EXPECT_EQ(mine[1].trade_id, trades[0].id);
}

TEST(OrderAmendmentCrossing, ARepriceThatFillsInFullEndsTheOrder) {
	order_book book;
	cross_seed(book, 10, 10);

	book.modify_order({.id = 1, .price = CROSS_ASK, .quantity = 10});

	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_FALSE(book.best_ask().has_value());

	std::vector<order_outcome> outcomes;
	book.cancel_order(1, outcomes);
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCEL_REJECTED)
		<< "the order left the book when it filled";
}

TEST(OrderAmendmentCrossing, APartialCrossRestsTheRemainderAtTheNewPrice) {
	order_book book;
	cross_seed(book, 10, 4);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = CROSS_ASK, .quantity = 10},
					  trades,
					  outcomes);

	ASSERT_EQ(trades.size(), 1U);
	EXPECT_EQ(trades[0].volume, 4);
	EXPECT_EQ(book.volume_at_price(CROSS_ASK, side_t::bid), 6)
		<< "six lots rest as a bid at 105";
	EXPECT_EQ(book.volume_at_price(CROSS_BID, side_t::bid), 0);
	EXPECT_FALSE(book.best_ask().has_value()) << "the offer was cleared";
}

// The lifecycle survives the amendment: it is the same order, so its fills
// accumulate rather than starting over. An order that reported 4-of-10 and
// then 4-of-6 would be telling a client about two different orders.
TEST(OrderAmendmentCrossing, TheTradedTotalCarriesAcrossAReprice) {
	order_book book;
	cross_seed(book, 10, 4);

	// Four lots off the bid first, so the order is already 4-of-10.
	(void)book.place_order(
		{.id = 3, .side = side_t::ask, .price = CROSS_BID, .qty = 4});

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = CROSS_ASK, .quantity = 10},
					  trades,
					  outcomes);

	const std::vector<order_outcome> mine = cross_for(outcomes, 1);
	ASSERT_EQ(mine.size(), 2U);
	EXPECT_EQ(mine[0].type, OutcomeType::MODIFIED);
	EXPECT_EQ(mine[0].traded, 4);
	EXPECT_EQ(mine[0].remaining, 6);
	EXPECT_EQ(mine[1].type, OutcomeType::FILL);
	EXPECT_EQ(mine[1].traded, 8) << "cumulative, not this fill's four";
	EXPECT_EQ(mine[1].remaining, 2);
}

// Both sides of an execution are reported, and an amendment is no exception -
// the resting order it hit gets its own record with its own quantities.
TEST(OrderAmendmentCrossing, BothSidesOfTheExecutionAreReported) {
	order_book book;
	cross_seed(book, 10, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = CROSS_ASK, .quantity = 4},
					  trades,
					  outcomes);

	const std::vector<order_outcome> maker = cross_for(outcomes, 2);
	ASSERT_EQ(maker.size(), 1U);
	EXPECT_EQ(maker[0].type, OutcomeType::FILL);
	EXPECT_EQ(maker[0].traded, 4);
	EXPECT_EQ(maker[0].remaining, 6);
	EXPECT_EQ(maker[0].trade_id, trades.front().id);
}

// A reprice that does not reach the other side is an ordinary move: it rests
// where it was told to and prints nothing.
TEST(OrderAmendmentCrossing, ARepriceInsideTheSpreadDoesNotTrade) {
	order_book book;
	cross_seed(book, 10, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = 104, .quantity = 10},
					  trades,
					  outcomes);

	EXPECT_TRUE(trades.empty());
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::MODIFIED);
	EXPECT_EQ(book.best_bid(), 104U);
	EXPECT_EQ(book.best_ask(), CROSS_ASK);
}

// An amendment carries no time-in-force because a resting order carries none:
// every resting order is GOOD_TILL_CANCELLED as far as matching is concerned,
// so an unfilled remainder rests rather than being dropped the way an IOC's
// would be.
TEST(OrderAmendmentCrossing, TheAmendedOrderRestsRatherThanBeingDropped) {
	order_book book;
	ASSERT_TRUE(
		book.place_order({.id   = 1,
						  .side = side_t::bid,
						  .tif = time_in_force_instruction::GOOD_TILL_CANCELLED,
						  .price = CROSS_BID,
						  .qty   = 10})
			.empty());

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = 101, .quantity = 10},
					  trades,
					  outcomes);

	ASSERT_EQ(outcomes.size(), 1U)
		<< "a MODIFIED and nothing else - no CANCELLED for a dropped tail";
	EXPECT_EQ(outcomes[0].type, OutcomeType::MODIFIED);
	EXPECT_EQ(book.volume_at_price(101, side_t::bid), 10);
}
