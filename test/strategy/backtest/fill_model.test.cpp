#include "strategy/backtest/fill_model.hpp"

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

	std::size_t infer() { return model.infer(replica, orders, out); }
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

// A locked market is the case the default is conservative about: we would have
// been behind whatever was already queued at that price, and an L2 feed carries
// no queue position that could say otherwise.
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
	fixture.model.retire_finished(fixture.orders);
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
	ASSERT_EQ(model.infer(replica, orders, out), 1U);
	EXPECT_EQ(placed(out, 0).qty, 2);
}
