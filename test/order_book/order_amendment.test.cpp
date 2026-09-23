#include "order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

// What an amendment costs an order, which is the only interesting question a
// MODIFY asks. Every case here is one row of modify_order's priority table:
// a downsize keeps its place in the queue, an increase gives it up, a reprice
// gives it up and lands at the back of the new level, and a downsize to at or
// below what has already executed is not an amendment at all.
//
// The crossing half - what happens when the new price is inside the spread -
// is order_amendment_crossing.test.cpp.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::orders;

namespace {

constexpr price_t AMEND_PRICE = 100;

/// @brief Rest one identified GTC bid, failing the test if it traded.
void amend_rest(order_book &book, order_id_t id, quantity_t qty,
				price_t price = AMEND_PRICE) {
	const std::vector<trade> trades = book.place_order(
		{.id = id, .side = side_t::bid, .price = price, .qty = qty});
	ASSERT_TRUE(trades.empty()) << "order " << id << " was meant to rest";
}

/// @brief Sell @p qty into the bid side at @p price and return the prints.
std::vector<trade> amend_sweep(order_book &book, quantity_t qty,
							   price_t price = AMEND_PRICE) {
	return book.place_order(
		{.id = 9000, .side = side_t::ask, .price = price, .qty = qty});
}

/// @brief Which resting order the sweep reached first.
order_id_t amend_first_filled(const std::vector<trade> &trades) {
	return trades.empty() ? 0 : trades.front().resting;
}

/// @brief The sole outcome, or a failure if there was not exactly one.
const order_outcome &amend_only(const std::vector<order_outcome> &all) {
	EXPECT_EQ(all.size(), 1U);
	return all.front();
}

} // namespace

// --------------------------------------------------------------------------
// Same price
// --------------------------------------------------------------------------

// The row Emporia's simulator gets right, and the reason a downsize is not
// routed through cancel-replace: the lots that remain are the same lots, and
// they have been queueing since the order was placed.
TEST(OrderAmendment, ADownsizeAtTheSamePriceKeepsQueuePosition) {
	order_book book;
	amend_rest(book, 1, 10);
	amend_rest(book, 2, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 4},
					  trades,
					  outcomes);

	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 14);
	const std::vector<trade> prints = amend_sweep(book, 4);
	ASSERT_EQ(prints.size(), 1U);
	EXPECT_EQ(amend_first_filled(prints), 1U) << "order 1 is still the head";
}

// The row Emporia gets wrong. Added lots arrived now, not when the order did,
// and a venue that let them jump the queue would let a one-lot order placed at
// the open be amended to a thousand at the touch.
TEST(OrderAmendment, AnIncreaseAtTheSamePriceGoesToTheBackOfTheQueue) {
	order_book book;
	amend_rest(book, 1, 10);
	amend_rest(book, 2, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 20},
					  trades,
					  outcomes);

	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 30);
	const std::vector<trade> prints = amend_sweep(book, 10);
	ASSERT_EQ(prints.size(), 1U);
	EXPECT_EQ(amend_first_filled(prints), 2U)
		<< "order 2 waited without changing its mind and now fills first";
}

// An amendment that asks for what the order already has is not an increase, so
// there is nothing to charge it for.
TEST(OrderAmendment, AnAmendmentToTheSameQuantityKeepsQueuePosition) {
	order_book book;
	amend_rest(book, 1, 10);
	amend_rest(book, 2, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 10},
					  trades,
					  outcomes);

	EXPECT_EQ(amend_only(outcomes).type, OutcomeType::MODIFIED);
	EXPECT_EQ(amend_first_filled(amend_sweep(book, 1)), 1U);
}

TEST(OrderAmendment, AnAppliedAmendmentIsReportedOnce) {
	order_book book;
	amend_rest(book, 1, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 6},
					  trades,
					  outcomes);

	const order_outcome &record = amend_only(outcomes);
	EXPECT_EQ(record.id, 1U);
	EXPECT_EQ(record.type, OutcomeType::MODIFIED);
	EXPECT_EQ(record.reason, reject_reason::NONE);
	EXPECT_EQ(record.status, OrderStatus::LIVE);
	EXPECT_EQ(record.traded, 0);
	EXPECT_EQ(record.remaining, 6);
	EXPECT_TRUE(trades.empty());
}

// The executed quantity is history and an amendment does not rewrite it: a
// 10-lot order that filled 4 and is amended to 6 has 2 left, not 6.
TEST(OrderAmendment, ADownsizeCountsFromWhatTheOrderHasExecuted) {
	order_book book;
	amend_rest(book, 1, 10);
	(void)amend_sweep(book, 4);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 6},
					  trades,
					  outcomes);

	const order_outcome &record = amend_only(outcomes);
	EXPECT_EQ(record.type, OutcomeType::MODIFIED);
	EXPECT_EQ(record.status, OrderStatus::PARTIALLY_FILLED);
	EXPECT_EQ(record.traded, 4);
	EXPECT_EQ(record.remaining, 2);
	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 2);
}

// --------------------------------------------------------------------------
// A price change
// --------------------------------------------------------------------------

TEST(OrderAmendment, ARepriceMovesTheOrderToTheNewLevel) {
	order_book book;
	amend_rest(book, 1, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = 99, .quantity = 10}, trades, outcomes);

	EXPECT_EQ(amend_only(outcomes).type, OutcomeType::MODIFIED);
	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 0)
		<< "nothing is left behind at the old price";
	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 10);
	EXPECT_EQ(book.best_bid(), 99U);
}

// The index has one entry per order, and a reprice rewrites it. If the old node
// were left linked it would keep resting and filling with no cancel able to
// reach it - the duplicate-id failure, arriving by another door.
TEST(OrderAmendment, ARepricedOrderIsStillCancellableByItsId) {
	order_book book;
	amend_rest(book, 1, 10);

	book.modify_order({.id = 1, .price = 99, .quantity = 10});

	std::vector<order_outcome> outcomes;
	book.cancel_order(1, outcomes);

	EXPECT_EQ(amend_only(outcomes).type, OutcomeType::CANCELLED);
	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 0);
	EXPECT_FALSE(book.best_bid().has_value()) << "the whole side is empty";
}

TEST(OrderAmendment, ARepriceLosesQueuePositionAtThePriceItReturnsTo) {
	order_book book;
	amend_rest(book, 1, 10);
	amend_rest(book, 2, 10);

	// Away and back: order 1 was the head, and rejoining is joining the back.
	book.modify_order({.id = 1, .price = 99, .quantity = 10});
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 10});

	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 20);
	EXPECT_EQ(amend_first_filled(amend_sweep(book, 10)), 2U);
}

// --------------------------------------------------------------------------
// Amendments that are not amendments
// --------------------------------------------------------------------------

TEST(OrderAmendment, ADownsizeToTheTradedQuantityIsACancel) {
	order_book book;
	amend_rest(book, 1, 10);
	(void)amend_sweep(book, 4);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 4},
					  trades,
					  outcomes);

	const order_outcome &record = amend_only(outcomes);
	EXPECT_EQ(record.type, OutcomeType::CANCELLED)
		<< "there is nothing left to amend, only something left to withdraw";
	EXPECT_EQ(record.status, OrderStatus::CANCELLED);
	EXPECT_EQ(record.traded, 4);
	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 0);
}

TEST(OrderAmendment, ADownsizeBelowTheTradedQuantityIsAlsoACancel) {
	order_book book;
	amend_rest(book, 1, 10);
	(void)amend_sweep(book, 4);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 1},
					  trades,
					  outcomes);

	EXPECT_EQ(amend_only(outcomes).type, OutcomeType::CANCELLED);
	EXPECT_EQ(outcomes.front().traded, 4) << "the fills are not undone";
}

TEST(OrderAmendment, AnAmendmentForAnOrderThatIsNotRestingIsDeclined) {
	order_book book;
	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;

	book.modify_order({.id = 7, .price = AMEND_PRICE, .quantity = 5},
					  trades,
					  outcomes);

	const order_outcome &record = amend_only(outcomes);
	EXPECT_EQ(record.type, OutcomeType::MODIFY_REJECTED);
	EXPECT_EQ(record.reason, reject_reason::UNKNOWN_ORDER);
}

// The fill/amend race, and it resolves the way the fill/cancel race does: one
// empty index probe cannot tell a filled order from one that never existed.
TEST(OrderAmendment, AnAmendmentForAnOrderThatFilledIsDeclined) {
	order_book book;
	amend_rest(book, 1, 10);
	(void)amend_sweep(book, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 20},
					  trades,
					  outcomes);

	EXPECT_EQ(amend_only(outcomes).type, OutcomeType::MODIFY_REJECTED);
	EXPECT_EQ(outcomes.front().reason, reject_reason::UNKNOWN_ORDER);
}

// Zero is not a cancel. There is no representable order_state for a
// non-positive order, and asking to become one is malformed rather than a
// withdrawal - the same boundary place_order enforces.
TEST(OrderAmendment, ANonPositiveAmendmentIsDeclinedAndChangesNothing) {
	order_book book;
	amend_rest(book, 1, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 1, .price = AMEND_PRICE, .quantity = 0},
					  trades,
					  outcomes);

	const order_outcome &record = amend_only(outcomes);
	EXPECT_EQ(record.type, OutcomeType::MODIFY_REJECTED);
	EXPECT_EQ(record.reason, reject_reason::NON_POSITIVE_QUANTITY);
	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 10);
}

TEST(OrderAmendment, AnAmendmentNamingTheAnonymousIdReportsNothing) {
	order_book book;
	book.add_order(side_t::bid, AMEND_PRICE, 10);

	std::vector<trade> trades;
	std::vector<order_outcome> outcomes;
	book.modify_order({.id = 0, .price = 99, .quantity = 5}, trades, outcomes);

	EXPECT_TRUE(outcomes.empty()) << "anonymous depth has nobody to report to";
	EXPECT_EQ(book.volume_at_price(AMEND_PRICE, side_t::bid), 10);
}
