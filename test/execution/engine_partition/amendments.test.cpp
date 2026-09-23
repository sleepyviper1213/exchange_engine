#include "engine_partition.fixture.hpp"
#include "execution/order_manager.hpp"
#include "orders/amendment.hpp"

#include <gtest/gtest.h>

// A MODIFY through the whole partition, which is where the book and the record
// store have to agree about it. The book owns every priority decision; what is
// pinned here is that the venue's *record* of the order follows - because the
// new price is on the command and not on the outcome, so the one piece of
// reconciliation that cannot be driven from the outcome stream lives in
// matching_engine::modify and nowhere else.
//
// The priority rules themselves are OrderAmendment; this is the wiring.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange::engine::orders;
using engine_partition_test::Engine;

namespace {

/// @brief Place one resting GTC bid for @p id, then drain.
void partition_rest(Engine &engine, order_id_t id, price_t price,
					quantity_t qty) {
	ASSERT_TRUE(engine.submit(command::place(
		{.id = id, .side = side_t::bid, .price = price, .qty = qty})));
	ASSERT_EQ(engine.drain(), 1U);
}

} // namespace

TEST(EnginePartitionAmendments, TheRecordFollowsAnAppliedAmendment) {
	Engine engine(nullptr);
	partition_rest(engine, 1, 100, 10);

	ASSERT_TRUE(engine.submit(
		command::modify(0, amendment{.id = 1, .price = 99, .quantity = 6})));
	ASSERT_EQ(engine.drain(), 1U);

	const order_record *record = engine.orders().find_record(1);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->price, 99U) << "the price only the command carried";
	EXPECT_EQ(record->state.quantity(), 6);
	EXPECT_EQ(record->state.remaining(), 6);
	EXPECT_EQ(status(*record), OrderStatus::LIVE);
	EXPECT_EQ(engine.orders().live(), 1U) << "an amendment does not retire one";

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::MODIFIED);
	EXPECT_EQ(engine.outcomes()[0].id, 1U);
}

// A declined amendment leaves the record exactly where it was, which is the
// whole point of declining it.
TEST(EnginePartitionAmendments, ADeclinedAmendmentLeavesTheRecordAlone) {
	Engine engine(nullptr);
	partition_rest(engine, 1, 100, 10);

	ASSERT_TRUE(engine.submit(
		command::modify(0, amendment{.id = 1, .price = 99, .quantity = 0})));
	ASSERT_EQ(engine.drain(), 1U);

	const order_record *record = engine.orders().find_record(1);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->price, 100U);
	EXPECT_EQ(record->state.quantity(), 10);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::MODIFY_REJECTED);
	EXPECT_EQ(engine.outcomes()[0].reason,
			  reject_reason::NON_POSITIVE_QUANTITY);
}

// A downsize to at or below what executed is a withdrawal, so the record has to
// retire rather than resize. The book reports it as a CANCELLED and reconcile's
// existing arm does the rest - which is why this is worth a case: the two
// halves are written in different files and only agree by contract.
TEST(EnginePartitionAmendments, ADownsizeToTheTradedQuantityRetiresTheRecord) {
	Engine engine(nullptr);
	partition_rest(engine, 1, 100, 10);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::ask, .price = 100, .qty = 4})));
	ASSERT_EQ(engine.drain(), 1U);

	// A fresh drain, so the buffers below hold this command's records alone.
	ASSERT_TRUE(engine.submit(
		command::modify(0, amendment{.id = 1, .price = 100, .quantity = 4})));
	ASSERT_EQ(engine.drain(), 1U);

	const order_record *record = engine.orders().find_record(1);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(status(*record), OrderStatus::CANCELLED);
	EXPECT_EQ(record->state.traded(), 4) << "the fills are not undone";
	EXPECT_EQ(engine.orders().live(), 0U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::CANCELLED);
}

// The record is driven *to* the totals the client was told, so a reprice that
// executes leaves the two unable to disagree - reconcile takes the increment as
// the difference between the outcome's cumulative traded and the record's.
TEST(EnginePartitionAmendments, ARepriceThatTradesKeepsTheRecordInStep) {
	Engine engine(nullptr);
	partition_rest(engine, 1, 100, 10);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::ask, .price = 105, .qty = 4})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_TRUE(engine.submit(
		command::modify(0, amendment{.id = 1, .price = 105, .quantity = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.trades().size(), 1U);
	EXPECT_EQ(engine.trades()[0].volume, 4);

	const order_record *amended = engine.orders().find_record(1);
	ASSERT_NE(amended, nullptr);
	EXPECT_EQ(amended->price, 105U);
	EXPECT_EQ(amended->state.traded(), 4);
	EXPECT_EQ(amended->state.remaining(), 6);

	const order_record *maker = engine.orders().find_record(2);
	ASSERT_NE(maker, nullptr);
	EXPECT_EQ(status(*maker), OrderStatus::FILLED);
}

// UNKNOWN_SYMBOL rather than UNKNOWN_ORDER: the order may well exist, on the
// partition this amendment should have reached, and telling the client it is
// unknown would send them looking in the wrong place.
TEST(EnginePartitionAmendments, AMisroutedAmendmentSaysTheSymbolIsWrong) {
	Engine engine(nullptr);

	ASSERT_TRUE(engine.submit(
		command::modify(99, amendment{.id = 1, .price = 100, .quantity = 5})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].type, OutcomeType::MODIFY_REJECTED);
	EXPECT_EQ(engine.outcomes()[0].id, 1U);
	EXPECT_EQ(engine.outcomes()[0].reason, reject_reason::UNKNOWN_SYMBOL);
}

// Every record a command produces is stamped with that command's ordinal,
// rejections included - a client correlating an answer to what it sent needs
// the number most when the answer is "no".
TEST(EnginePartitionAmendments, EveryRecordAnAmendmentProducesIsSequenced) {
	Engine engine(nullptr);
	partition_rest(engine, 1, 100, 10);

	ASSERT_TRUE(engine.submit(
		command::modify(0, amendment{.id = 1, .price = 99, .quantity = 12})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_EQ(engine.outcomes().size(), 1U);
	EXPECT_EQ(engine.outcomes()[0].sequence, 2U)
		<< "the second command applied";
}
