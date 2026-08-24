#include "execution/dispatcher.hpp"
#include "execution/engine_partition.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <vector>

// Which listings a partition answers for, and what happens to a command that
// names one it does not carry. A partition refuses a symbol it was never given
// rather than inventing a book for it, because a dispatcher and a
// reference-data set that disagree must produce a visible rejection and not an
// order resting where nothing will ever match it.

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;

namespace {

// The partition refuses a symbol it was not given rather than inventing a book,
// so a dispatcher and a reference-data set that disagree produce a visible
// rejection instead of an order resting where nothing will ever match it.
TEST(EnginePartitionListings, ACommandForAnUnregisteredListingIsRejected) {
	engine_partition<256> partition(nullptr);
	partition.listing(1); // carries symbol 1 only

	ASSERT_TRUE(partition.submit(event::command::place({.id        = 7,
														.symbol_id = 2,
														.side  = side_t::bid,
														.price = 100,
														.qty   = 10})));
	EXPECT_EQ(partition.drain(), 1u);

	EXPECT_EQ(partition.misrouted(), 1u);
	ASSERT_EQ(partition.outcomes().size(), 1u);
	EXPECT_EQ(partition.outcomes()[0].id, 7u);
	EXPECT_EQ(partition.outcomes()[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(partition.outcomes()[0].reason, reject_reason::UNKNOWN_SYMBOL);
	EXPECT_EQ(partition.books().size(), 1u); // nothing was created for it
}

// UNKNOWN_SYMBOL, not UNKNOWN_ORDER: the order may well exist, on the partition
// this cancel should have reached. Saying the order is unknown would send the
// client looking in the wrong place.
TEST(EnginePartitionListings,
	 AMisroutedCancelSaysTheSymbolIsUnknownNotTheOrder) {
	engine_partition<256> partition(nullptr);
	partition.listing(1);

	ASSERT_TRUE(partition.submit(event::command::cancel(2, 7)));
	EXPECT_EQ(partition.drain(), 1u);

	ASSERT_EQ(partition.outcomes().size(), 1u);
	EXPECT_EQ(partition.outcomes()[0].type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(partition.outcomes()[0].reason, reject_reason::UNKNOWN_SYMBOL);
}

// Depth carries no identity, so there is nobody to report a misroute to - the
// counter is the only place it shows up.
TEST(EnginePartitionListings, MisroutedDepthIsCountedButProducesNoOutcome) {
	engine_partition<256> partition(nullptr);
	partition.listing(1);

	ASSERT_TRUE(partition.submit(event::command::add(2, side_t::bid, 100, 10)));
	ASSERT_TRUE(
		partition.submit(event::command::reduce(2, side_t::ask, 101, 5)));
	EXPECT_EQ(partition.drain(), 2u);

	EXPECT_EQ(partition.misrouted(), 2u);
	EXPECT_TRUE(partition.outcomes().empty());
}

// One partition, several listings, no cross-talk: that is the whole point of
// the engine looking a book up per command instead of owning one.
TEST(EnginePartitionListings, ListingsOnOnePartitionDoNotSeeEachOther) {
	engine_partition<256> partition(nullptr);
	partition.listing(1);
	partition.listing(2);

	ASSERT_TRUE(partition.submit(event::command::add(1, side_t::bid, 100, 10)));
	ASSERT_TRUE(partition.submit(event::command::add(2, side_t::bid, 100, 3)));
	EXPECT_EQ(partition.drain(), 2u);
	EXPECT_EQ(partition.misrouted(), 0u);

	ASSERT_NE(partition.book(1), nullptr);
	ASSERT_NE(partition.book(2), nullptr);
	EXPECT_EQ(partition.book(1)->volume_at_price(100, side_t::bid), 10);
	EXPECT_EQ(partition.book(2)->volume_at_price(100, side_t::bid), 3);
}

// An order for one listing must not cross against another's depth, even at the
// same price - the two books never meet.
TEST(EnginePartitionListings, OrdersDoNotCrossBetweenListings) {
	engine_partition<256> partition(nullptr);
	partition.listing(1);
	partition.listing(2);

	ASSERT_TRUE(partition.submit(event::command::add(1, side_t::ask, 100, 10)));
	ASSERT_TRUE(partition.submit(event::command::place({.id        = 5,
														.symbol_id = 2,
														.side  = side_t::bid,
														.price = 100,
														.qty   = 10})));
	EXPECT_EQ(partition.drain(), 2u);

	EXPECT_TRUE(partition.trades().empty()) << "listings must not cross";
	EXPECT_EQ(partition.book(1)->volume_at_price(100, side_t::ask), 10);
	EXPECT_EQ(partition.book(2)->volume_at_price(100, side_t::bid), 10);
}

// A listing registered after the fact starts working; nothing has to be
// rebuilt.
TEST(EnginePartitionListings, RegisteringAListingLaterMakesItsCommandsLand) {
	engine_partition<256> partition(nullptr);

	ASSERT_TRUE(partition.submit(event::command::add(4, side_t::bid, 100, 10)));
	EXPECT_EQ(partition.drain(), 1u);
	EXPECT_EQ(partition.misrouted(), 1u);

	partition.listing(4);
	ASSERT_TRUE(partition.submit(event::command::add(4, side_t::bid, 100, 10)));
	EXPECT_EQ(partition.drain(), 1u);

	EXPECT_EQ(partition.misrouted(), 1u); // still the one from before
	ASSERT_NE(partition.book(4), nullptr);
	EXPECT_EQ(partition.book(4)->volume_at_price(100, side_t::bid), 10);
}

// The dispatcher decides who owns a listing; the partition that owns it takes
// the command and the others refuse it. Together that is the routing contract.
TEST(EnginePartitionListings, OnlyTheOwningPartitionAcceptsACommand) {
	constexpr std::size_t PARTITIONS = 4;
	const dispatcher route(PARTITIONS);
	std::vector<std::unique_ptr<engine_partition<256>>> partitions;
	partitions.reserve(PARTITIONS);
	for (std::size_t i = 0; i < PARTITIONS; ++i)
		partitions.push_back(std::make_unique<engine_partition<256>>(nullptr));

	for (symbol_id_t symbol = 0; symbol < 12; ++symbol)
		partitions[route.partition_for(symbol)]->listing(symbol);

	constexpr symbol_id_t SYMBOL = 6;
	const auto cmd = event::command::add(SYMBOL, side_t::bid, 100, 10);

	for (std::size_t index = 0; index < PARTITIONS; ++index) {
		ASSERT_TRUE(partitions[index]->submit(cmd));
		EXPECT_EQ(partitions[index]->drain(), 1u);
	}

	const std::size_t owner = route.partition_for(SYMBOL);
	for (std::size_t index = 0; index < PARTITIONS; ++index) {
		if (index == owner) {
			EXPECT_EQ(partitions[index]->misrouted(), 0u);
			EXPECT_EQ(
				partitions[index]->book(SYMBOL)->volume_at_price(100,
																 side_t::bid),
				10);
		} else {
			EXPECT_EQ(partitions[index]->misrouted(), 1u) << index;
			EXPECT_EQ(partitions[index]->book(SYMBOL), nullptr) << index;
		}
	}
}

} // namespace
