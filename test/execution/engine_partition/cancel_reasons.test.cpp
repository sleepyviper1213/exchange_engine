#include "engine_partition.fixture.hpp"
#include "execution/order_manager.hpp"

#include <gtest/gtest.h>

// The cancel/fill race, told properly. order_book answers every unapplicable
// cancel with UNKNOWN_ORDER, because its index holds resting orders only and one
// empty probe covers "filled a microsecond ago", "already cancelled" and "never
// placed" alike. The record store kept all three, so the partition can say which
// it was - and must still say UNKNOWN_ORDER where it genuinely cannot tell.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using engine_partition_test::Engine;

namespace {

/// @brief The single outcome the last drain produced, or a fatal test failure.
[[nodiscard]] order_outcome only_outcome(const Engine &engine) {
	EXPECT_EQ(engine.outcomes().size(), 1U);
	return engine.outcomes().empty() ? order_outcome{} : engine.outcomes()[0];
}

} // namespace

// The ordinary path, unchanged: a resting order cancels, and the reason stays
// NONE because a client cancel needs no excuse.
TEST(EnginePartitionCancelReasons, ARestingOrderStillCancelsCleanly) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);

	const order_outcome answer = only_outcome(engine);
	EXPECT_EQ(answer.type, OutcomeType::CANCELLED);
	EXPECT_EQ(answer.reason, reject_reason::NONE);
	EXPECT_FALSE(engine.book(0)->best_bid().has_value());
	EXPECT_EQ(status(*engine.orders().find_record(1)), OrderStatus::CANCELLED);
}

// The race the store exists to resolve: the cancel arrived behind a fill. Under
// the book alone this is UNKNOWN_ORDER and the client is left wondering whether
// the order ever existed.
TEST(EnginePartitionCancelReasons, CancellingAFilledOrderSaysItFilled) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::ask, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 2, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_EQ(engine.drain(), 2U);

	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);

	const order_outcome answer = only_outcome(engine);
	EXPECT_EQ(answer.type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(answer.reason, reject_reason::ORDER_ALREADY_FILLED);
}

TEST(EnginePartitionCancelReasons, CancellingTwiceSaysItWasAlreadyCancelled) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 2U);

	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);

	const order_outcome answer = only_outcome(engine);
	EXPECT_EQ(answer.type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(answer.reason, reject_reason::ORDER_ALREADY_CANCELLED);
}

// A rejected order never entered the book, which is a different thing to tell a
// client than "it was withdrawn" - one may be re-sent, the other may have traded.
TEST(EnginePartitionCancelReasons, CancellingARejectedOrderSaysItWasRejected) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::place(
		{.id  = 1,
		 .side = side_t::bid,
		 .tif  = orders::time_in_force_instruction::FILL_OR_KILL,
		 .price = 100,
		 .qty   = 10})));
	ASSERT_EQ(engine.drain(), 1U);

	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);

	const order_outcome answer = only_outcome(engine);
	EXPECT_EQ(answer.type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(answer.reason, reject_reason::ORDER_ALREADY_REJECTED);
}

TEST(EnginePartitionCancelReasons, CancellingAnIdNobodyPlacedIsStillUnknown) {
	Engine engine(nullptr);
	ASSERT_TRUE(engine.submit(command::cancel(0, 404)));
	ASSERT_EQ(engine.drain(), 1U);

	const order_outcome answer = only_outcome(engine);
	EXPECT_EQ(answer.type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(answer.reason, reject_reason::UNKNOWN_ORDER);
}

// The honest limit. History is bounded, and once a record has aged out the
// partition knows exactly as much as the book did - so it says exactly what the
// book would have.
TEST(EnginePartitionCancelReasons, AnOrderAgedOutOfHistoryIsUnknownAgain) {
	Engine engine(nullptr, nullptr, 1U << 10, 2U); // room for two records
	ASSERT_TRUE(engine.submit(command::place(
		{.id = 1, .side = side_t::bid, .price = 100, .qty = 10})));
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 2U);
	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);
	ASSERT_EQ(only_outcome(engine).reason,
			  reject_reason::ORDER_ALREADY_CANCELLED);

	// Two more orders, both finished, push record 1 out of the two-slot store.
	for (order_id_t id = 2; id <= 3; ++id) {
		ASSERT_TRUE(engine.submit(command::place(
			{.id = id, .side = side_t::bid, .price = 100, .qty = 10})));
		ASSERT_TRUE(engine.submit(command::cancel(0, id)));
		ASSERT_EQ(engine.drain(), 2U);
	}
	ASSERT_FALSE(engine.orders().contains(1));

	ASSERT_TRUE(engine.submit(command::cancel(0, 1)));
	ASSERT_EQ(engine.drain(), 1U);
	EXPECT_EQ(only_outcome(engine).reason, reject_reason::UNKNOWN_ORDER);
}

// A misroute is answered before the store is consulted at all: the order may
// well be alive, on the partition this cancel should have reached, and telling
// the client anything about *this* partition's records would send them looking
// in the wrong place.
TEST(EnginePartitionCancelReasons, AMisroutedCancelStillSaysUnknownSymbol) {
	Engine engine(nullptr); // carries symbol 0 only
	ASSERT_TRUE(engine.submit(command::cancel(5, 1)));
	ASSERT_EQ(engine.drain(), 1U);

	const order_outcome answer = only_outcome(engine);
	EXPECT_EQ(answer.type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(answer.reason, reject_reason::UNKNOWN_SYMBOL);
	EXPECT_EQ(engine.misrouted(), 1U);
}
