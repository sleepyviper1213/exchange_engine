#include "strategy/backtest/fill_model.hpp"

#include "strategy/backtest/order_manager_view.hpp"

#include "backtest.fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// The passive fill inference. Every case here is a statement about when the
// model may say one of our orders traded, and about how much - the two
// questions a depth-only recording cannot answer on its own, and therefore the
// two that every other number in a run rests on.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy::backtest;

using orders::time_in_force_instruction;

namespace {

/// @brief The model, its sink, and the two books it reads, in one place.
struct model_under_test {
	symbol_spec spec = unit_listing();
	recording_sink sink;
	execution::order_manager orders{64};
	market_data::l2_book replica;
	std::vector<command> out;
	crossing_fill_model<recording_sink> model;

	explicit model_under_test(fill_model_options options = {})
		: model(sink, spec, options) {
		model.open_step();
	}

	/// @brief Tell the model an order was placed, the way a submission would.
	void submitted(order_id_t id, side_t side, price_t price, quantity_t qty) {
		rest(orders, id, side, price, qty);
		EXPECT_TRUE(model.submit(command::place(orders::order{.id        = id,
															  .symbol_id = 0,
															  .side      = side,
															  .price = price,
															  .qty   = qty})));
	}

	std::size_t infer() {
		return model.infer(replica, order_manager_view{orders}, out);
	}
};

} // namespace

TEST(BacktestFillModel, InfersNothingWithoutOrdersOfOurs) {
	model_under_test fixture;
	fixture.replica.set_level(side_t::ask, 100, 50);
	EXPECT_EQ(fixture.infer(), 0U);
}

TEST(BacktestFillModel, InfersNothingAgainstAnEmptyReplica) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	EXPECT_EQ(fixture.infer(), 0U);
}

// The whole point of the model: our bid is not filled by the venue merely
// quoting near it, only by the venue offering through it.
TEST(BacktestFillModel, LeavesARestingBidAloneWhileTheVenueQuotesAbove) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::ask, 101, 50);
	EXPECT_EQ(fixture.infer(), 0U);
}

TEST(BacktestFillModel, FillsARestingBidWhenTheVenueOffersBelowIt) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::ask, 99, 4);

	ASSERT_EQ(fixture.infer(), 1U);
	const orders::order &aggressor = placed(fixture.out, 0);
	EXPECT_EQ(aggressor.id, 0U) << "the venue's side is anonymous, so it takes "
								   "no record and reports no outcome";
	EXPECT_EQ(aggressor.side, side_t::ask);
	EXPECT_EQ(aggressor.price, 100U)
		<< "a passive order fills at its own limit";
	EXPECT_EQ(aggressor.qty, 4) << "bounded by what the venue published";
	EXPECT_EQ(aggressor.tif, time_in_force_instruction::IMMEDIATE_OR_CANCEL)
		<< "a remainder that rested would become depth nobody published";
}

TEST(BacktestFillModel, FillsARestingAskWhenTheVenueBidsAboveIt) {
	model_under_test fixture;
	fixture.submitted(1, side_t::ask, 100, 10);
	fixture.replica.set_level(side_t::bid, 101, 25);

	ASSERT_EQ(fixture.infer(), 1U);
	const orders::order &aggressor = placed(fixture.out, 0);
	EXPECT_EQ(aggressor.side, side_t::bid);
	EXPECT_EQ(aggressor.price, 100U);
	EXPECT_EQ(aggressor.qty, 10) << "bounded by our own size this time";
}

// A locked market is the case the default is conservative about: a venue offer
// *at* our price is not on its own evidence that anything traded. The queue
// model below sharpens this rather than replacing it - the two rules overlap
// deliberately, and both defaults stay pessimistic. @see fill_model_options
TEST(BacktestFillModel, RefusesALockedMarketByDefault) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::ask, 100, 50);
	EXPECT_EQ(fixture.infer(), 0U);
}

TEST(BacktestFillModel, FillsALockedMarketWhenAskedTo) {
	model_under_test fixture{
		fill_model_options{.require_trade_through = false}};
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::ask, 100, 6);

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 6);
}

// The liquidity budget. Two prices of ours and only enough published size for
// the better one and a lot over - the deeper price may have the remainder and
// no more, because liquidity offered below 99 is a subset of that offered
// below 100.
TEST(BacktestFillModel, SharesOnePublishedSizeAcrossOurPricesBestFirst) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 5);
	fixture.submitted(2, side_t::bid, 99, 5);
	fixture.replica.set_level(side_t::ask, 98, 6);

	ASSERT_EQ(fixture.infer(), 2U);
	EXPECT_EQ(placed(fixture.out, 0).price, 100U) << "best price fills first";
	EXPECT_EQ(placed(fixture.out, 0).qty, 5);
	EXPECT_EQ(placed(fixture.out, 1).price, 99U);
	EXPECT_EQ(placed(fixture.out, 1).qty, 1) << "all the venue had left";
}

TEST(BacktestFillModel, StopsAtTheWorstPriceTheLiquidityReaches) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 5);
	fixture.submitted(2, side_t::bid, 99, 5);
	// Offered below 100 but not below 99: the deeper quote is not crossed at
	// all.
	fixture.replica.set_level(side_t::ask, 99, 20);

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).price, 100U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 5);
}

// Without this the harness would not terminate: the replica does not move
// between settle rounds, so a partially filled order would keep finding the
// same untouched depth and keep filling against it.
TEST(BacktestFillModel, DoesNotFillTwiceAgainstOnePublishedSize) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::ask, 99, 4);

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(fixture.infer(), 0U)
		<< "the budget is spent until the next event";
}

TEST(BacktestFillModel, ReleasesTheBudgetOnTheNextEvent) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::ask, 99, 4);

	ASSERT_EQ(fixture.infer(), 1U);
	fixture.model.open_step();
	EXPECT_EQ(fixture.infer(), 1U);
}

TEST(BacktestFillModel, CountsWhatItInjected) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::ask, 99, 4);
	ASSERT_EQ(fixture.infer(), 1U);

	EXPECT_EQ(fixture.model.injected(), 1U);
	EXPECT_EQ(fixture.model.injected_lots(), 4);
}

// A refused batch must leave the model exactly as it found it, or the gate's
// rollback would hand the same order back and it would be tracked twice.
TEST(BacktestFillModel, RecordsNothingWhenTheSinkRefuses) {
	model_under_test fixture;
	fixture.sink.refuse(true);
	rest(fixture.orders, 1, side_t::bid, 100, 10);
	EXPECT_FALSE(
		fixture.model.submit(command::place(orders::order{.id        = 1,
														  .symbol_id = 0,
														  .side  = side_t::bid,
														  .price = 100,
														  .qty   = 10})));
	EXPECT_EQ(fixture.model.working(), 0U);
}

TEST(BacktestFillModel, TracksOnlyIdentifiedOrders) {
	model_under_test fixture;
	EXPECT_TRUE(fixture.model.submit(command::add(0, side_t::bid, 100, 10)));
	EXPECT_TRUE(fixture.model.submit(command::cancel(0, 7)));
	EXPECT_EQ(fixture.model.working(), 0U);
	EXPECT_EQ(fixture.sink.size(), 2U) << "both still reach the sink";
}

TEST(BacktestFillModel, DropsOrdersTheVenueHasFinishedWith) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	ASSERT_EQ(fixture.model.working(), 1U);

	fixture.orders.cancel(fixture.orders.find(1));
	fixture.model.retire_finished(order_manager_view{fixture.orders});
	EXPECT_EQ(fixture.model.working(), 0U);
}

TEST(BacktestFillModel, IgnoresTheFilledPartOfAWorkingOrder) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.orders.apply_fill(fixture.orders.find(1), 7);
	fixture.replica.set_level(side_t::ask, 99, 50);

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 3) << "only the remainder is exposed";
}

// The venue's sizes are on its own scale and ours are on the listing's lot
// grid; the two need not line up. A bound is the one place truncating is right,
// because refusing the level would say the venue offered nothing at all.
TEST(BacktestFillModel, RoundsThePublishedSizeDownToWholeLots) {
	const symbol_spec coarse{0, "TEST", 0, 0, 1, 10, 100};
	recording_sink sink;
	execution::order_manager orders{64};
	market_data::l2_book replica;
	crossing_fill_model<recording_sink> model(sink, coarse);
	model.open_step();

	rest(orders, 1, side_t::bid, 100, 10);
	ASSERT_TRUE(model.submit(command::place(orders::order{.id        = 1,
														  .symbol_id = 0,
														  .side  = side_t::bid,
														  .price = 100,
														  .qty   = 10})));
	replica.set_level(side_t::ask, 99, 25); // 2.5 lots at a lot size of 10

	std::vector<command> out;
	ASSERT_EQ(model.infer(replica, order_manager_view{orders}, out), 1U);
	EXPECT_EQ(placed(out, 0).qty, 2);
}

// --- queue position ---------------------------------------------------------
//
// Everything above leaves the venue publishing nothing at our own price, which
// is why the suite reads as it did before the queue existed: the estimate is
// zero and absorbs nothing. These are the cases where the venue was quoting
// where we are, and being second in line is the whole difference.
//
// Note the shape they all take. The queue is *measured* on a frame where our
// price is behind the venue's touch, and *spent* on a later frame where the
// touch has come through it. It cannot be both in one frame - a replica in
// sequence is never crossed or locked - and that is the reason for the two
// steps rather than a convenience of the fixture. @see reconcile_queue

// Measured, then spent: twenty lots of the venue's own were ahead of our bid at
// 100, so the twenty-five that later traded through pay those off first and
// only the remaining five reach us.
TEST(BacktestFillModel, FillsOnlyWhatIsLeftOnceTheQueueAheadIsPaidDown) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U) << "nothing has traded through us yet";
	ASSERT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 20);

	// The market trades down: the bid at 100 is gone and the offer is at 99.
	fixture.replica.set_level(side_t::bid, 100, 0);
	fixture.replica.set_level(side_t::ask, 101, 0);
	fixture.replica.set_level(side_t::ask, 99, 25);
	fixture.model.open_step();

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 5) << "25 offered, 20 of it in front";
	EXPECT_EQ(fixture.model.queue().absorbed_lots(), 20);
	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 0);
}

// The same recording with the model turned off: first in line, filled in full.
// This is the assumption the harness made everywhere before, and the gap
// between the two numbers is what the feature is worth.
TEST(BacktestFillModel, FillsInFullWhenTheQueueIsNotModelled) {
	model_under_test fixture{fill_model_options{.model_queue_position = false}};
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);

	fixture.replica.set_level(side_t::bid, 100, 0);
	fixture.replica.set_level(side_t::ask, 101, 0);
	fixture.replica.set_level(side_t::ask, 99, 25);
	fixture.model.open_step();

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 10) << "our whole size, immediately";
	EXPECT_EQ(fixture.model.queue().absorbed_lots(), 0);
}

// Not enough came through to reach us at all.
TEST(BacktestFillModel, DoesNotFillWhileTheQueueAheadOutlastsTheVolume) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);

	fixture.replica.set_level(side_t::bid, 100, 0);
	fixture.replica.set_level(side_t::ask, 101, 0);
	fixture.replica.set_level(side_t::ask, 99, 6);
	fixture.model.open_step();

	EXPECT_EQ(fixture.infer(), 0U);
	EXPECT_EQ(fixture.model.queue().absorbed_lots(), 6);
	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 14);
}

// And the progress that buys: what was absorbed stays absorbed, so the next
// event starts from where the last one left off rather than from the back.
TEST(BacktestFillModel, WorksItsWayForwardThroughTheQueueAcrossEvents) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);

	fixture.replica.set_level(side_t::bid, 100, 0);
	fixture.replica.set_level(side_t::ask, 101, 0);
	fixture.replica.set_level(side_t::ask, 99, 12);
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U) << "twelve of the twenty ahead of us";
	ASSERT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 8);

	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 4)
		<< "eight paid off the rest of the queue, four reached us";
}

// The one inference a depth feed supports while we are still behind the touch:
// the level shrinking below the queue we recorded proves the queue shrank.
TEST(BacktestFillModel, TightensTheQueueWhenTheVenuesLevelShrinks) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);
	ASSERT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 20);

	fixture.replica.set_level(side_t::bid, 100, 3);
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U);
	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 3);
}

// A level growing grows at the back, so it says nothing about our position.
TEST(BacktestFillModel, IgnoresLiquidityThatJoinedTheLevelBehindUs) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);

	fixture.replica.set_level(side_t::bid, 100, 500);
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U);
	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 20);
}

// The rule that makes the class do anything at all. Once the venue's offer has
// come through our price, the bid we were queued behind is missing because the
// market moved past it - not because it was cancelled - so the estimate must
// stand rather than be re-measured against a level that is necessarily gone.
TEST(BacktestFillModel, HoldsTheQueueOnceTheTouchHasComeThroughOurPrice) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);

	// The venue can no longer be bidding 100 while offering 99 - that book
	// would be crossed, and the reconstructor tears a crossed replica down.
	fixture.replica.set_level(side_t::bid, 100, 0);
	fixture.replica.set_level(side_t::ask, 101, 0);
	fixture.replica.set_level(side_t::ask, 99, 4);
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U);

	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 16)
		<< "re-measuring here would read the whole queue as cancelled and put "
		   "us at the front on exactly the frame the estimate is needed";
}

// A torn-down replica publishes nothing, and reading that as an empty queue
// would hand us the front of every price for free.
TEST(BacktestFillModel, LeavesTheQueueAloneWhenTheReplicaPublishesNothing) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);
	ASSERT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 20);

	fixture.replica.set_level(side_t::bid, 100, 0);
	fixture.replica.set_level(side_t::ask, 101, 0);
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U);

	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 20)
		<< "the absence of evidence is not evidence of an empty queue";
}

// What the session does on a gap instead: void the estimates outright, which
// re-measures a surviving order from the back of whatever comes back.
TEST(BacktestFillModel, ReMeasuresTheQueueAfterAReset) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);
	ASSERT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 20);

	fixture.model.reset_queue();
	fixture.replica.set_level(side_t::bid, 100, 35);
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U);
	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 35)
		<< "a fresh join, at the back of the level the new snapshot shows";
}

// Leaving a price and returning to it must join again rather than inherit.
TEST(BacktestFillModel, ForgetsTheQueueAtAPriceWeStopQuoting) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 100, 20);
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);
	ASSERT_EQ(fixture.model.queue().tracked(), 1U);

	fixture.orders.cancel(fixture.orders.find(1));
	fixture.model.retire_finished(order_manager_view{fixture.orders});
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U);
	EXPECT_EQ(fixture.model.queue().tracked(), 0U);

	fixture.replica.set_level(side_t::bid, 100, 7);
	fixture.submitted(2, side_t::bid, 100, 10);
	fixture.model.open_step();
	ASSERT_EQ(fixture.infer(), 0U);
	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 7)
		<< "a new order at that price is at the back of it";
}

// Both sides queue independently, and an ask queues behind the venue's asks.
TEST(BacktestFillModel, QueuesARestingAskBehindTheVenuesOwnAsks) {
	model_under_test fixture;
	fixture.submitted(1, side_t::ask, 100, 10);
	fixture.replica.set_level(side_t::ask, 100, 6);
	fixture.replica.set_level(side_t::bid, 99, 50);
	ASSERT_EQ(fixture.infer(), 0U);
	ASSERT_EQ(fixture.model.queue().ahead(side_t::ask, 100), 6);

	fixture.replica.set_level(side_t::ask, 100, 0);
	fixture.replica.set_level(side_t::bid, 99, 0);
	fixture.replica.set_level(side_t::bid, 101, 9);
	fixture.model.open_step();

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 3) << "nine bid, six of it in front";
}

// The queue that matters is the one at *our* price, not the whole book.
TEST(BacktestFillModel, QueuesOnlyBehindLiquidityAtOurOwnPrice) {
	model_under_test fixture;
	fixture.submitted(1, side_t::bid, 100, 10);
	fixture.replica.set_level(side_t::bid, 98, 1000); // worse, and irrelevant
	fixture.replica.set_level(side_t::ask, 101, 50);
	ASSERT_EQ(fixture.infer(), 0U);
	EXPECT_EQ(fixture.model.queue().ahead(side_t::bid, 100), 0);

	fixture.replica.set_level(side_t::ask, 101, 0);
	fixture.replica.set_level(side_t::ask, 99, 4);
	fixture.model.open_step();

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 4);
}

// An order resting at a price the venue's touch has already passed got there by
// taking everything in front of it, so it really is first in line.
TEST(BacktestFillModel, JoinsAtTheFrontOfAPriceAlreadyThroughTheTouch) {
	model_under_test fixture;
	fixture.replica.set_level(side_t::bid, 98, 50);
	fixture.replica.set_level(side_t::ask, 99, 4);
	fixture.submitted(1, side_t::bid, 100, 10);

	ASSERT_EQ(fixture.infer(), 1U);
	EXPECT_EQ(placed(fixture.out, 0).qty, 4)
		<< "no measurement was possible and none was needed";
	EXPECT_EQ(fixture.model.queue().absorbed_lots(), 0);
}
