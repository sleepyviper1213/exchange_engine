#include "engine_partition.fixture.hpp"
#include "execution/order_manager.hpp"

#include <gtest/gtest.h>

// What the partition remembers about an order after the book has finished with
// it. The book and the record store are two different objects and the matching
// engine keeps them in step from the outcome stream - these suites are what
// says the two agree, on both sides of a fill and down every path an order can
// take out of the book.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using engine_partition_test::Engine;

// Both sides of one fill, and the maker is the interesting half: nothing hands
// the engine the maker's handle, so its record moves only because reconcile()
// looks every outcome's id up.
TEST(EnginePartitionRecords, ARecordTracksBothSidesOfAFill) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 4})));
	ASSERT_EQ(engine.drain(), 2U);

	const order_record *maker = engine.orders().find_record(1);
	ASSERT_NE(maker, nullptr);
	EXPECT_EQ(status(*maker), OrderStatus::PARTIALLY_FILLED);
	EXPECT_EQ(maker->state.traded(), 4);
	EXPECT_EQ(maker->state.remaining(), 6);
	EXPECT_EQ(maker->side, side_t::ask);
	EXPECT_EQ(maker->price, 100U);

	const order_record *taker = engine.orders().find_record(2);
	ASSERT_NE(taker, nullptr);
	EXPECT_EQ(status(*taker), OrderStatus::FILLED);
	EXPECT_EQ(taker->state.traded(), 4);
	EXPECT_EQ(taker->state.remaining(), 0);

	// The maker is still resting and can still be acted on; the taker is done.
	EXPECT_EQ(engine.orders().live(), 1U);
	EXPECT_EQ(engine.orders().retained(), 1U);
	EXPECT_EQ(engine.orders().high_water(), 2U);
}

// Anonymous liquidity belongs to nobody, so there is nothing to record and
// nobody to report to - the same rule the book applies to id 0.
TEST(EnginePartitionRecords, AnAnonymousPlaceLeavesNoRecord) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 0, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	EXPECT_EQ(engine.orders().size(), 0U);
	EXPECT_TRUE(engine.outcomes().empty());
	// It did rest, though: no record is not the same as no order.
	EXPECT_EQ(engine.book(0)->volume_at_price(100, side_t::ask), 10);
}

// A misroute must not leave a record either, or the partition that *should*
// have taken the order would find its id already spent here.
TEST(EnginePartitionRecords, AMisroutedPlaceLeavesNoRecord) {
	Engine engine(nullptr); // carries symbol 0 only
	ASSERT_TRUE(engine.submit(command::place({.id        = 9,
											  .symbol_id = 5,
											  .side      = side_t::bid,
											  .price     = 100,
											  .qty       = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	EXPECT_EQ(engine.misrouted(), 1U);
	EXPECT_EQ(engine.orders().size(), 0U);
	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].reason, reject_reason::UNKNOWN_SYMBOL);
}

// The book refused an order the store had already admitted. The record has to
// follow the book's answer, not the admission's optimism.
TEST(EnginePartitionRecords, AFillOrKillTheBookRefusesIsRecordedAsRejected) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(
		command::place({.id   = 1,
						.side = side_t::bid,
						.tif  = orders::time_in_force_instruction::FILL_OR_KILL,
						.price = 100,
						.qty   = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	const order_record *record = engine.orders().find_record(1);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(status(*record), OrderStatus::REJECTED);
	EXPECT_EQ(record->reason, reject_reason::INSUFFICIENT_LIQUIDITY);
	EXPECT_EQ(record->state.traded(), 0);
	EXPECT_FALSE(is_active(*record));
	EXPECT_EQ(engine.orders().live(), 0U);
	EXPECT_EQ(engine.orders().retained(), 1U);
}

// An IOC that fills in part: the executed quantity stands and only the
// remainder is withdrawn, with the instruction named as the cause.
TEST(EnginePartitionRecords, ADroppedIocRemainderIsRecordedAsCancelled) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 4})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id    = 2,
		 .side  = side_t::bid,
		 .tif   = orders::time_in_force_instruction::IMMEDIATE_OR_CANCEL,
		 .price = 100,
		 .qty   = 10})));
	ASSERT_EQ(engine.drain(), 2U);

	const order_record *record = engine.orders().find_record(2);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(status(*record), OrderStatus::CANCELLED);
	EXPECT_EQ(record->reason, reject_reason::TIME_IN_FORCE);
	EXPECT_EQ(record->state.traded(), 4);
	EXPECT_EQ(record->state.remaining(), 6);
}

// The behaviour the store exists for. The book forgets a filled order and would
// take its id again; the store remembers, so the second order under that id is
// refused instead of quietly becoming a second lifecycle under one name.
TEST(EnginePartitionRecords, AnIdIsRefusedOnceItsOrderHasFinished) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 2U);
	ASSERT_EQ(engine.orders().live(), 0U) << "both orders filled";

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 101, .qty = 5})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(engine.outcomes()[0].reason, reject_reason::DUPLICATE_ORDER_ID);
	// Refused before the book saw it, so nothing rests at the new price.
	EXPECT_FALSE(engine.book(0)->best_ask().has_value());
}

// Ids are unique within a session, not for all time, and clearing the store is
// what starts the next one.
TEST(EnginePartitionRecords, ClearingTheStoreLetsASessionsIdsBeUsedAgain) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 2U);

	engine.orders().clear();
	EXPECT_EQ(engine.orders().size(), 0U);

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 101, .qty = 5})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::ACCEPTED);
	EXPECT_EQ(engine.orders().live(), 1U);
	// The capacity reading is a lifetime one and survives the session boundary.
	EXPECT_EQ(engine.orders().high_water(), 2U);
}

// One store serves every listing the partition carries, because a client order
// id names an order at the venue and not at an instrument.
TEST(EnginePartitionRecords, OneStoreSpansEveryListingThePartitionCarries) {
	Engine engine(nullptr);
	engine.listing(1);

	ASSERT_TRUE(engine.submit(command::place({.id        = 1,
											  .symbol_id = 0,
											  .side      = side_t::bid,
											  .price     = 100,
											  .qty       = 10})));
	ASSERT_TRUE(engine.submit(command::place({.id        = 2,
											  .symbol_id = 1,
											  .side      = side_t::bid,
											  .price     = 200,
											  .qty       = 10})));
	ASSERT_EQ(engine.drain(), 2U);

	ASSERT_NE(engine.orders().find_record(1), nullptr);
	ASSERT_NE(engine.orders().find_record(2), nullptr);
	EXPECT_EQ(engine.orders().find_record(1)->symbol, 0U);
	EXPECT_EQ(engine.orders().find_record(2)->symbol, 1U);
	EXPECT_EQ(engine.orders().live(), 2U);

	// And the id is spent across the whole partition, not just its own listing.
	ASSERT_TRUE(engine.submit(command::place({.id        = 1,
											  .symbol_id = 1,
											  .side      = side_t::bid,
											  .price     = 200,
											  .qty       = 10})));
	ASSERT_EQ(engine.drain(), 1U);
	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].reason, reject_reason::DUPLICATE_ORDER_ID);
}

// REDUCE was the one command that could put the book and the store out of step:
// it drains a level without regard to identity and emits no outcome, so an
// identified order it destroyed left a record still believing the order was
// live. order_book::delete_order now walks past identified orders, so the two
// cannot diverge - this is the sequence that used to prove they could.
TEST(EnginePartitionRecords, AReductionCannotSilentlyDestroyAClientsOrder) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 100, .qty = 5})));
	ASSERT_TRUE(engine.submit(command::add(0, side_t::bid, 100, 6)));
	ASSERT_TRUE(engine.submit(command::reduce(0, side_t::bid, 100, 11)));
	ASSERT_EQ(engine.drain(), 3U);

	// The anonymous depth went; the client's order did not.
	EXPECT_EQ(engine.book(0)->volume_at_price(100, side_t::bid), 5);
	const order_record *record = engine.orders().find_record(1);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(status(*record), OrderStatus::LIVE);
	EXPECT_EQ(engine.orders().live(), 1U);

	// And the store is telling the truth: the order really is still
	// cancellable, so the cancel is applied rather than declined.
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);
	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::CANCELLED);
	EXPECT_EQ(status(*engine.orders().find_record(1)), OrderStatus::CANCELLED);
}

// A store with no room for another live order refuses rather than forgetting
// one, and the refusal reaches the client as its own reason.
TEST(EnginePartitionRecords, AFullStoreRefusesRatherThanForgettingALiveOrder) {
	// Two records, and both orders rest, so neither can be recycled.
	Engine engine(nullptr, nullptr, 1U << 10, 2U);
	for (order_id_t id = 1; id <= 2; ++id)
		ASSERT_TRUE(engine.submit(command::place(
			{.id = id, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 2U);
	ASSERT_EQ(engine.orders().live(), 2U);

	ASSERT_TRUE(engine.submit(command::place(
		{.id = 3, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(engine.outcomes()[0].reason, reject_reason::BOOK_AT_CAPACITY);
	// The two live orders are untouched, which is the point of refusing.
	EXPECT_EQ(engine.orders().live(), 2U);
	EXPECT_EQ(engine.book(0)->volume_at_price(100, side_t::bid), 20);
}

// An anonymous order that *matches* - not the seeding add_order, which rests
// without crossing, but a PLACE under the reserved id zero. It takes no record
// of its own and reports no outcome of its own, and the early return that
// encoded both used to skip reconciliation entirely. The resting orders such an
// order fills are identified, though, and their records have to move: leave
// them alone and the store believes an order is working after the book has
// finished with it, so a later cancel is told it is still live and every read
// of its remaining quantity is stale.
//
// Reachable since the backtest fill model, which injects the venue's side of a
// passive fill under this id precisely so it stays out of the order flow.
// @see strategy/backtest/fill_model.hpp
TEST(EnginePartitionRecords, AnAnonymousAggressorStillRetiresWhatItFilled) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 1U);
	ASSERT_TRUE(is_active(*engine.orders().find_record(1)));

	// Anonymous, immediate-or-cancel, and it takes the whole resting order.
	ASSERT_TRUE(engine.submit(command::place(
		{.id    = 0,
		 .side  = side_t::ask,
		 .tif   = orders::time_in_force_instruction::IMMEDIATE_OR_CANCEL,
		 .price = 100,
		 .qty   = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	const order_record *maker = engine.orders().find_record(1);
	ASSERT_NE(maker, nullptr) << "the record is history, not gone";
	EXPECT_EQ(maker->state.traded(), 10);
	EXPECT_EQ(maker->state.remaining(), 0);
	EXPECT_EQ(status(*maker), OrderStatus::FILLED);
	EXPECT_FALSE(is_active(*maker));
	EXPECT_EQ(engine.orders().live(), 0U)
		<< "nothing is working; the store must not still be holding a slot";
	EXPECT_EQ(engine.orders().cancellable(1),
			  reject_reason::ORDER_ALREADY_FILLED)
		<< "a late cancel must be told the order filled, not that it is live";
}

TEST(EnginePartitionRecords, AnAnonymousAggressorTakesNoRecordOfItsOwn) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id    = 0,
		 .side  = side_t::ask,
		 .tif   = orders::time_in_force_instruction::IMMEDIATE_OR_CANCEL,
		 .price = 100,
		 .qty   = 4})));
	ASSERT_EQ(engine.drain(), 2U);

	EXPECT_EQ(engine.orders().size(), 1U)
		<< "only the identified order is kept";
	EXPECT_EQ(engine.orders().find_record(1)->state.remaining(), 6);
}
