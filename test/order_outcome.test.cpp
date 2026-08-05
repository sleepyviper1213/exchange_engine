#include "trading-engine/order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

// The order lifecycle as the book reports it, ported from Emporia's
// verification/order-lifecycle/OrderLifecycle.tla. Where order_state.test.cpp
// covers one order's arithmetic, this covers the parts only a book can have:
// which transitions actually occur, in what order, and which requests the book
// declines.
//
// Every test here is a fate that produced no output at all before: a rejected
// order, a dropped remainder, a cancel that lost its race.

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;

namespace {

/// @brief Only the outcomes concerning @p id, in the order they were emitted.
std::vector<OrderOutcome> for_order(const std::vector<OrderOutcome> &all,
									order_id_t id) {
	std::vector<OrderOutcome> mine;
	for (const OrderOutcome &o : all)
		if (o.id == id) mine.push_back(o);
	return mine;
}

} // namespace

// --------------------------------------------------------------------------
// Acceptance (TLA+: Accept)
// --------------------------------------------------------------------------

TEST(OrderOutcomes, RestingOrderIsAcknowledgedOnce) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].id, 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::ACCEPTED);
	EXPECT_EQ(outcomes[0].status, OrderStatus::LIVE);
	EXPECT_EQ(outcomes[0].traded, 0);
	EXPECT_EQ(outcomes[0].remaining, 10);
	EXPECT_TRUE(trades.empty());
}

TEST(OrderOutcomes, AnonymousOrdersReportNothing) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	// id 0 is the anonymous sentinel: no client to report to, no index entry.
	ob.place_order({.id = 0, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.add_order(side_t::ask, 200, 5);

	EXPECT_TRUE(outcomes.empty());
}

// --------------------------------------------------------------------------
// Rejection at the validation boundary (TLA+: Reject)
// --------------------------------------------------------------------------

TEST(OrderOutcomes, NonPositiveQuantityIsRejectedAndLeavesTheBookUntouched) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 0},
				   trades,
				   outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::NON_POSITIVE_QUANTITY);
	EXPECT_EQ(outcomes[0].status, OrderStatus::REJECTED);
	EXPECT_FALSE(ob.best_bid().has_value());
}

// Admitting a second order under a live id would overwrite its index entry and
// leave the first one resting but uncancellable.
TEST(OrderOutcomes, DuplicateIdIsRejectedAndTheFirstOrderSurvives) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	outcomes.clear();
	ob.place_order({.id = 1, .side = side_t::bid, .price = 99, .qty = 7},
				   trades,
				   outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::DUPLICATE_ORDER_ID);
	EXPECT_EQ(ob.volume_at_price(99, side_t::bid), 0);

	// The first order is still there and still reachable by id.
	outcomes.clear();
	ob.cancel_order(1, outcomes);
	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCELLED);
	EXPECT_EQ(ob.volume_at_price(100, side_t::bid), 0);
}

// A stop order that rested immediately would be a live order the client never
// asked for, so the book declines it until something watches the trigger.
TEST(OrderOutcomes, AStopOrderIsRefusedRatherThanRestedLikeALimit) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id         = 1,
					.side       = side_t::bid,
					.type       = order_type::STOP,
					.price      = 100,
					.stop_price = 105,
					.qty        = 10},
				   trades,
				   outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::UNSUPPORTED_ORDER_TYPE);
	EXPECT_FALSE(ob.best_bid().has_value()); // nothing rested
	EXPECT_TRUE(trades.empty());
}

TEST(OrderOutcomes, UnfillableFillOrKillIsRejectedWithoutTrading) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;
	ob.add_order(side_t::ask, 100, 4); // only 4 available

	ob.place_order({.id    = 1,
					.side  = side_t::bid,
					.tif   = time_in_force_instruction::FILL_OR_KILL,
					.price = 100,
					.qty   = 10},
				   trades,
				   outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::INSUFFICIENT_LIQUIDITY);
	EXPECT_EQ(outcomes[0].remaining, 10);
	EXPECT_TRUE(trades.empty());
	EXPECT_EQ(ob.volume_at_price(100, side_t::ask), 4); // untouched
}

// --------------------------------------------------------------------------
// Execution (TLA+: ApplyFill)
// --------------------------------------------------------------------------

TEST(OrderOutcomes, BothSidesOfAFillAreReported) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	outcomes.clear();
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);

	const auto resting   = for_order(outcomes, 1);
	const auto aggressor = for_order(outcomes, 2);

	ASSERT_EQ(resting.size(), 1U);
	EXPECT_EQ(resting[0].type, OutcomeType::FILL);
	EXPECT_EQ(resting[0].status, OrderStatus::FILLED);
	EXPECT_EQ(resting[0].traded, 10);
	EXPECT_EQ(resting[0].remaining, 0);

	ASSERT_EQ(aggressor.size(), 2U);
	EXPECT_EQ(aggressor[0].type, OutcomeType::ACCEPTED);
	EXPECT_EQ(aggressor[1].type, OutcomeType::FILL);
	EXPECT_EQ(aggressor[1].status, OrderStatus::FILLED);
	EXPECT_EQ(aggressor[1].traded, 10);
}

TEST(OrderOutcomes, PartialFillLeavesTheRestingOrderPartiallyFilled) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	outcomes.clear();
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 4},
				   trades,
				   outcomes);

	const auto resting = for_order(outcomes, 1);
	ASSERT_EQ(resting.size(), 1U);
	EXPECT_EQ(resting[0].status, OrderStatus::PARTIALLY_FILLED);
	EXPECT_EQ(resting[0].traded, 4);
	EXPECT_EQ(resting[0].remaining, 6);
}

// The one that decides whether the resting remainder continues the aggressor's
// lifecycle or restarts it: a 10-lot order that fills 4 and rests 6 must report
// 10 traded when the rest fills, not 6.
TEST(OrderOutcomes, RestedRemainderKeepsTheOrdersCumulativeQuantities) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.add_order(side_t::ask, 100, 4); // anonymous liquidity to cross into
	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);

	auto mine = for_order(outcomes, 1);
	ASSERT_EQ(mine.size(), 2U);
	EXPECT_EQ(mine[1].type, OutcomeType::FILL);
	EXPECT_EQ(mine[1].traded, 4);
	EXPECT_EQ(mine[1].remaining, 6); // 6 now resting

	// Hit the remainder; the report is against the original 10, not the 6.
	outcomes.clear();
	ob.place_order({.id = 2, .side = side_t::ask, .price = 100, .qty = 6},
				   trades,
				   outcomes);
	mine = for_order(outcomes, 1);
	ASSERT_EQ(mine.size(), 1U);
	EXPECT_EQ(mine[0].status, OrderStatus::FILLED);
	EXPECT_EQ(mine[0].traded, 10);
	EXPECT_EQ(mine[0].remaining, 0);
}

TEST(OrderOutcomes, ImmediateOrCancelRemainderIsCancelledWithTimeInForce) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;
	ob.add_order(side_t::ask, 100, 4);

	ob.place_order({.id    = 1,
					.side  = side_t::bid,
					.tif   = time_in_force_instruction::IMMEDIATE_OR_CANCEL,
					.price = 100,
					.qty   = 10},
				   trades,
				   outcomes);

	const auto mine = for_order(outcomes, 1);
	ASSERT_EQ(mine.size(), 3U);
	EXPECT_EQ(mine[0].type, OutcomeType::ACCEPTED);
	EXPECT_EQ(mine[1].type, OutcomeType::FILL);
	EXPECT_EQ(mine[2].type, OutcomeType::CANCELLED);
	EXPECT_EQ(mine[2].reason, reject_reason::TIME_IN_FORCE);
	EXPECT_EQ(mine[2].traded, 4);    // the executed part is kept
	EXPECT_EQ(mine[2].remaining, 6); // the dropped part is reported
	EXPECT_FALSE(ob.best_bid().has_value()); // nothing rested
}

// --------------------------------------------------------------------------
// Cancellation and the cancel/fill race
// (TLA+: ConfirmCancel / DeclineCancelAfterFill)
// --------------------------------------------------------------------------

TEST(OrderOutcomes, CancelConfirmsAndKeepsTheExecutedQuantity) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 4},
				   trades,
				   outcomes);
	outcomes.clear();

	ob.cancel_order(1, outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCELLED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::NONE);
	EXPECT_EQ(outcomes[0].status, OrderStatus::CANCELLED);
	// NoExecutionAfterCancellation: the 4 already executed stay executed.
	EXPECT_EQ(outcomes[0].traded, 4);
	EXPECT_EQ(outcomes[0].remaining, 6);
	EXPECT_FALSE(ob.best_ask().has_value());
}

TEST(OrderOutcomes, CancellingAnOrderThatAlreadyFilledIsDeclined) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	outcomes.clear();

	// The fill won the race; order 1 left the book before the cancel landed.
	ob.cancel_order(1, outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].id, 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::UNKNOWN_ORDER);
}

TEST(OrderOutcomes, CancellingAnUnknownIdIsDeclined) {
	order_book ob;
	std::vector<OrderOutcome> outcomes;

	ob.cancel_order(42, outcomes);

	ASSERT_EQ(outcomes.size(), 1U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(outcomes[0].reason, reject_reason::UNKNOWN_ORDER);
}

TEST(OrderOutcomes, CancellingTwiceDeclinesTheSecondRequest) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	outcomes.clear();

	ob.cancel_order(1, outcomes);
	ob.cancel_order(1, outcomes);

	ASSERT_EQ(outcomes.size(), 2U);
	EXPECT_EQ(outcomes[0].type, OutcomeType::CANCELLED);
	EXPECT_EQ(outcomes[1].type, OutcomeType::CANCEL_REJECTED);
}

// Every cancel request resolves exactly once, whichever way it goes — the
// liveness property `CancelRequestEventuallyResolves` reduced to the
// synchronous case, where "eventually" is "before the call returns".
TEST(OrderOutcomes, EveryCancelRequestProducesExactlyOneOutcome) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	outcomes.clear();

	for (const order_id_t id : {1U, 1U, 2U, 99U}) ob.cancel_order(id, outcomes);
	EXPECT_EQ(outcomes.size(), 4U);
}

// --------------------------------------------------------------------------
// Cross-cutting: an id's stream is a valid path through the state machine
// --------------------------------------------------------------------------

// TerminalStatusNeverChanges: once an order reports FILLED, CANCELLED or
// REJECTED, nothing further may be reported for it.
TEST(OrderOutcomes, NothingIsReportedAfterATerminalOutcome) {
	order_book ob;
	std::vector<Trade> trades;
	std::vector<OrderOutcome> outcomes;

	ob.place_order({.id = 1, .side = side_t::ask, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	ob.place_order({.id = 2, .side = side_t::bid, .price = 100, .qty = 10},
				   trades,
				   outcomes);
	// order 1 is FILLED. Anything aimed at it now must be declined, not applied.
	ob.cancel_order(1, outcomes);

	bool seen_terminal = false;
	for (const OrderOutcome &o : for_order(outcomes, 1)) {
		EXPECT_FALSE(seen_terminal && o.type != OutcomeType::CANCEL_REJECTED)
			<< "outcome after a terminal state: " << to_string(o.type);
		if (is_terminal(o.status)) seen_terminal = true;
	}
	EXPECT_TRUE(seen_terminal);
}
