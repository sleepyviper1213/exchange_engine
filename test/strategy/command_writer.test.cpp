#include "strategy/command_writer.hpp"

#include "event/command.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"

#include <gtest/gtest.h>

#include <cstddef>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy;
using exchange::engine::event::command;

namespace {

constexpr symbol_id_t SYMBOL = 42;

TEST(CommandWriter, StartsEmptyWithFullCapacity) {
	command_batch<4> batch(SYMBOL);

	EXPECT_TRUE(batch.writer().empty());
	EXPECT_EQ(batch.writer().size(), 0U);
	EXPECT_EQ(batch.writer().remaining(), 4U);
	EXPECT_EQ(batch.writer().symbol(), SYMBOL);
	EXPECT_TRUE(batch.view().empty());
}

TEST(CommandWriter, StampsItsSymbolOnEveryCommandKind) {
	command_batch<4> batch(SYMBOL);
	command_writer &out = batch.writer();

	out.place({.id = 1, .side = side_t::bid, .price = 100, .qty = 5});
	out.cancel(1);
	out.add(side_t::ask, 101, 7);
	out.reduce(side_t::ask, 101, 3);

	ASSERT_EQ(batch.view().size(), 4U);
	for (const command &cmd : batch.view()) EXPECT_EQ(cmd.symbol, SYMBOL);
}

// The symbol on the order itself is overwritten, not merely accompanied: a
// PLACE routes on command::symbol but the book reads order::symbol_id, and the
// two disagreeing is the bug this prevents.
TEST(CommandWriter, PlaceOverwritesTheOrdersOwnSymbol) {
	command_batch<1> batch(SYMBOL);

	batch.writer().place({.id         = 1,
						  .symbol_id  = 9999,
						  .side       = side_t::bid,
						  .price      = 100,
						  .qty        = 5});

	ASSERT_EQ(batch.view().size(), 1U);
	EXPECT_EQ(batch.view()[0].symbol, SYMBOL);
	EXPECT_EQ(batch.view()[0].as_place().symbol_id, SYMBOL);
}

TEST(CommandWriter, PreservesTheOrderItWasGiven) {
	command_batch<1> batch(SYMBOL);
	const orders::order sent{.id    = 77,
							 .side  = side_t::ask,
							 .type  = orders::order_type::LIMIT,
							 .tif   = orders::time_in_force_instruction::
								 IMMEDIATE_OR_CANCEL,
							 .price = 250,
							 .qty   = 12};

	batch.writer().place(sent);

	const orders::order &got = batch.view()[0].as_place();
	EXPECT_EQ(got.id, sent.id);
	EXPECT_EQ(got.side, sent.side);
	EXPECT_EQ(got.price, sent.price);
	EXPECT_EQ(got.qty, sent.qty);
	EXPECT_EQ(got.tif, sent.tif);
}

TEST(CommandWriter, CarriesTheRightPayloadPerKind) {
	command_batch<3> batch(SYMBOL);
	command_writer &out = batch.writer();

	out.cancel(31337);
	out.add(side_t::bid, 500, 25);
	out.reduce(side_t::ask, 600, 4);

	EXPECT_EQ(batch.view()[0].type, command::Type::CANCEL);
	EXPECT_EQ(batch.view()[0].as_cancel(), 31337U);

	EXPECT_EQ(batch.view()[1].type, command::Type::ADD);
	EXPECT_EQ(batch.view()[1].as_level().side, side_t::bid);
	EXPECT_EQ(batch.view()[1].as_level().price, 500U);
	EXPECT_EQ(batch.view()[1].as_level().volume, 25);

	EXPECT_EQ(batch.view()[2].type, command::Type::REDUCE);
	EXPECT_EQ(batch.view()[2].as_level().price, 600U);
	EXPECT_EQ(batch.view()[2].as_level().volume, 4);
}

TEST(CommandWriter, RemainingTracksWritesAndReachesZeroAtCapacity) {
	command_batch<3> batch(SYMBOL);
	command_writer &out = batch.writer();

	out.cancel(1);
	EXPECT_EQ(out.remaining(), 2U);
	out.cancel(2);
	EXPECT_EQ(out.remaining(), 1U);
	out.cancel(3);
	EXPECT_EQ(out.remaining(), 0U);
	EXPECT_EQ(out.size(), 3U);
}

TEST(CommandWriter, ResetEmptiesWithoutDisturbingCapacityOrSymbol) {
	command_batch<2> batch(SYMBOL);
	batch.writer().cancel(1);
	batch.writer().cancel(2);

	batch.writer().reset();

	EXPECT_TRUE(batch.writer().empty());
	EXPECT_EQ(batch.writer().remaining(), 2U);
	EXPECT_EQ(batch.writer().symbol(), SYMBOL);

	// And the storage is genuinely reusable, not merely reported as empty.
	batch.writer().cancel(3);
	ASSERT_EQ(batch.view().size(), 1U);
	EXPECT_EQ(batch.view()[0].as_cancel(), 3U);
}

// A view over the accumulated commands has to be a real contiguous range -
// spsc_queue::try_emplace_range requires it and takes its memcpy path on it.
TEST(CommandWriter, ViewIsContiguousOverWhatWasWritten) {
	command_batch<4> batch(SYMBOL);
	batch.writer().cancel(10);
	batch.writer().cancel(20);

	const auto view = batch.view();
	ASSERT_EQ(view.size(), 2U);
	EXPECT_EQ(view.data() + 1, &view[1]);
	EXPECT_EQ(view[0].as_cancel(), 10U);
	EXPECT_EQ(view[1].as_cancel(), 20U);
	EXPECT_EQ(batch.size(), 2U);
}

TEST(CommandWriter, CapacityIsTheTemplateArgument) {
	static_assert(command_batch<8>::CAPACITY == 8);
	static_assert(command_batch<1>::CAPACITY == 1);
	// Nothing here may relocate: the cursor points into the batch's own bytes.
	static_assert(!std::is_copy_constructible_v<command_batch<4>>);
	static_assert(!std::is_move_constructible_v<command_batch<4>>);
	SUCCEED();
}

} // namespace
