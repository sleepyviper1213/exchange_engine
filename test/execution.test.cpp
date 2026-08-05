#include "trading-engine/execution/book_manager.hpp"
#include "trading-engine/execution/dispatcher.hpp"
#include "trading-engine/execution/engine_partition.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <set>
#include <vector>

// The routing layer: which partition owns a listing, and who owns its book.
// Between them they are what turns N single-threaded matching engines into one
// engine, so the properties worth pinning are the ones other code assumes
// without checking — that a book's address never moves, that a symbol nobody
// registered answers null rather than an empty book, and that routing is a pure
// function two threads can compute independently and agree on.

using namespace exchange::engine;
using namespace exchange::engine::execution;
using namespace exchange;

// --------------------------------------------------------------------------
// book_manager
// --------------------------------------------------------------------------

TEST(BookManager, StartsEmptyAndFindsNothing) {
	book_manager books;
	EXPECT_TRUE(books.empty());
	EXPECT_EQ(books.size(), 0u);
	EXPECT_EQ(books.lookup(0), nullptr);
	EXPECT_EQ(books.lookup(7), nullptr);
	EXPECT_FALSE(books.contains(7));
}

TEST(BookManager, CreateThenLookupFindsTheSameBook) {
	book_manager books;
	order_book &created = books.create(3);

	EXPECT_EQ(books.lookup(3), &created);
	EXPECT_TRUE(books.contains(3));
	EXPECT_EQ(books.size(), 1u);
	EXPECT_FALSE(books.empty());
}

// A symbol this partition does not carry must read as absent, not as an empty
// book — otherwise a misroute becomes an order silently accepted onto a book
// nobody will ever read.
TEST(BookManager, LookupOfAnUnregisteredSymbolIsNullNotAFreshBook) {
	book_manager books;
	books.create(3);

	EXPECT_EQ(books.lookup(4), nullptr);   // inside the slot vector, unoccupied
	EXPECT_EQ(books.lookup(999), nullptr); // past its end
	EXPECT_EQ(books.size(), 1u);           // neither lookup created anything
}

// Replacing a live book would drop every order resting on it with nothing
// emitted to say so, so a second create returns the book already there.
TEST(BookManager, CreateIsIdempotentAndKeepsRestingOrders) {
	book_manager books;
	order_book &first = books.create(3);
	first.add_order(side_t::bid, 100, 10);

	order_book &again = books.create(3);

	EXPECT_EQ(&again, &first);
	EXPECT_EQ(books.size(), 1u);
	EXPECT_EQ(again.volume_at_price(100, side_t::bid), 10);
}

// The engine takes a reference from lookup and keeps using it. Registering more
// listings grows the slot vector, and that must not move the books.
TEST(BookManager, BookAddressesSurviveLaterRegistrations) {
	book_manager books;
	order_book &first = books.create(0);
	first.add_order(side_t::bid, 100, 10);

	for (symbol_id_t symbol = 1; symbol <= 64; ++symbol) books.create(symbol);

	EXPECT_EQ(books.lookup(0), &first);
	EXPECT_EQ(first.volume_at_price(100, side_t::bid), 10);
	EXPECT_EQ(books.size(), 65u);
}

TEST(BookManager, RemoveDropsTheBookAndReportsWhetherOneWasThere) {
	book_manager books;
	books.create(3);

	EXPECT_TRUE(books.remove(3));
	EXPECT_FALSE(books.contains(3));
	EXPECT_EQ(books.lookup(3), nullptr);
	EXPECT_EQ(books.size(), 0u);

	EXPECT_FALSE(books.remove(3));   // already gone
	EXPECT_FALSE(books.remove(999)); // never existed
}

// A hole is not a book: size counts listings, not slots.
TEST(BookManager, SizeCountsLiveBooksNotSlots) {
	book_manager books;
	books.create(0);
	books.create(10); // leaves slots 1..9 empty
	EXPECT_EQ(books.size(), 2u);

	books.remove(0);
	EXPECT_EQ(books.size(), 1u);
	EXPECT_TRUE(books.contains(10));
}

TEST(BookManager, ClearDropsEveryBookAndLeavesTheManagerReusable) {
	book_manager books;
	books.create(1);
	books.create(2);

	books.clear();

	EXPECT_TRUE(books.empty());
	EXPECT_EQ(books.lookup(1), nullptr);

	order_book &rebuilt = books.create(1);
	rebuilt.add_order(side_t::ask, 101, 5);
	EXPECT_EQ(books.size(), 1u);
	EXPECT_EQ(rebuilt.volume_at_price(101, side_t::ask), 5);
}

// --------------------------------------------------------------------------
// dispatcher
// --------------------------------------------------------------------------

TEST(Dispatcher, OnePartitionOwnsEverything) {
	const dispatcher route(1);
	EXPECT_EQ(route.partition_count(), 1u);
	for (symbol_id_t symbol = 0; symbol < 100; ++symbol)
		EXPECT_EQ(route.partition_for(symbol), 0u);
}

// Dense ids modulo the partition count deal out round-robin. That is the whole
// argument for not scrambling the id: adjacent listings land on *different*
// partitions, which a hash would only achieve on average.
TEST(Dispatcher, DenseSymbolsDealOutRoundRobin) {
	const dispatcher route(4);
	EXPECT_EQ(route.partition_for(0), 0u);
	EXPECT_EQ(route.partition_for(1), 1u);
	EXPECT_EQ(route.partition_for(2), 2u);
	EXPECT_EQ(route.partition_for(3), 3u);
	EXPECT_EQ(route.partition_for(4), 0u);
}

// The power-of-two mask and the modulo must be the same function, or a venue
// that picks 6 partitions gets different routing from one that picks 8.
TEST(Dispatcher, NonPowerOfTwoCountsRouteLikeAModulo) {
	const dispatcher route(6);
	EXPECT_EQ(route.partition_count(), 6u);
	for (symbol_id_t symbol = 0; symbol < 200; ++symbol)
		EXPECT_EQ(route.partition_for(symbol), symbol % 6u);
}

TEST(Dispatcher, EveryPartitionIsReachableAndNoneIsOutOfRange) {
	for (const std::size_t count : {1U, 2U, 3U, 5U, 8U, 16U}) {
		const dispatcher route(count);
		std::set<std::size_t> seen;
		for (symbol_id_t symbol = 0; symbol < 512; ++symbol) {
			const std::size_t partition = route.partition_for(symbol);
			ASSERT_LT(partition, count) << "count=" << count;
			seen.insert(partition);
		}
		EXPECT_EQ(seen.size(), count) << "count=" << count;
	}
}

TEST(Dispatcher, RoutingIsAPureFunctionOfTheSymbol) {
	const dispatcher route(8);
	const dispatcher same(8);
	for (symbol_id_t symbol = 0; symbol < 64; ++symbol) {
		EXPECT_EQ(route.partition_for(symbol), route.partition_for(symbol));
		EXPECT_EQ(route.partition_for(symbol), same.partition_for(symbol));
	}
}

// Every command type has to be routable, which is why the symbol sits outside
// the union. Before it did, only PLACE could be routed at all.
TEST(Dispatcher, RoutesEveryCommandTypeByItsSymbol) {
	const dispatcher route(4);
	constexpr symbol_id_t symbol = 6; // partition 2

	const auto place = event::command::place(
		{.id = 1, .symbol_id = symbol, .side = side_t::bid, .price = 100,
		 .qty = 10});
	const auto cancel = event::command::cancel(symbol, 1);
	const auto add    = event::command::add(symbol, side_t::bid, 100, 10);
	const auto reduce = event::command::reduce(symbol, side_t::bid, 100, 4);

	EXPECT_EQ(route.partition_for(place), 2u);
	EXPECT_EQ(route.partition_for(cancel), 2u);
	EXPECT_EQ(route.partition_for(add), 2u);
	EXPECT_EQ(route.partition_for(reduce), 2u);
}

// PLACE takes its routing key off the order rather than from a second argument,
// so the two can never disagree.
TEST(Dispatcher, PlaceRoutesByTheOrdersOwnSymbol) {
	const dispatcher route(4);
	const auto place = event::command::place(
		{.id = 1, .symbol_id = 9, .side = side_t::ask, .price = 100, .qty = 1});

	EXPECT_EQ(place.symbol, 9u);
	EXPECT_EQ(route.partition_for(place), route.partition_for(symbol_id_t{9}));
}

TEST(Dispatcher, OwnsAgreesWithPartitionFor) {
	const dispatcher route(4);
	for (symbol_id_t symbol = 0; symbol < 32; ++symbol) {
		const std::size_t owner = route.partition_for(symbol);
		for (std::size_t partition = 0; partition < 4; ++partition)
			EXPECT_EQ(route.owns(partition, symbol), partition == owner);
	}
}

// --------------------------------------------------------------------------
// The two together: routing a command reaches the book that command names.
// --------------------------------------------------------------------------

TEST(Routing, EachPartitionCarriesOnlyItsOwnListings) {
	constexpr std::size_t PARTITIONS = 4;
	const dispatcher route(PARTITIONS);
	std::vector<book_manager> partitions(PARTITIONS);

	// Reference data hands out 20 dense listings; each goes to its owner.
	for (symbol_id_t symbol = 0; symbol < 20; ++symbol)
		partitions[route.partition_for(symbol)].create(symbol);

	for (symbol_id_t symbol = 0; symbol < 20; ++symbol) {
		const std::size_t owner = route.partition_for(symbol);
		EXPECT_NE(partitions[owner].lookup(symbol), nullptr) << symbol;
		for (std::size_t other = 0; other < PARTITIONS; ++other)
			if (other != owner)
				EXPECT_EQ(partitions[other].lookup(symbol), nullptr) << symbol;
	}
}

TEST(Routing, ACommandFindsTheBookItNames) {
	const dispatcher route(4);
	std::vector<book_manager> partitions(4);
	constexpr symbol_id_t symbol = 6;

	partitions[route.partition_for(symbol)].create(symbol);

	const auto cmd = event::command::add(symbol, side_t::bid, 100, 10);
	order_book *book = partitions[route.partition_for(cmd)].lookup(cmd.symbol);

	ASSERT_NE(book, nullptr);
	book->add_order(cmd.level.side, cmd.level.price, cmd.level.volume);
	EXPECT_EQ(book->volume_at_price(100, side_t::bid), 10);
}

// --------------------------------------------------------------------------
// engine_partition: commands reaching (or missing) the listing they name
// --------------------------------------------------------------------------

// The partition refuses a symbol it was not given rather than inventing a book,
// so a dispatcher and a reference-data set that disagree produce a visible
// rejection instead of an order resting where nothing will ever match it.
TEST(EnginePartition, ACommandForAnUnregisteredListingIsRejected) {
	engine_partition<256> partition(nullptr);
	partition.listing(1); // carries symbol 1 only

	ASSERT_TRUE(partition.submit(event::command::place(
		{.id = 7, .symbol_id = 2, .side = side_t::bid, .price = 100,
		 .qty = 10})));
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
TEST(EnginePartition, AMisroutedCancelSaysTheSymbolIsUnknownNotTheOrder) {
	engine_partition<256> partition(nullptr);
	partition.listing(1);

	ASSERT_TRUE(partition.submit(event::command::cancel(2, 7)));
	EXPECT_EQ(partition.drain(), 1u);

	ASSERT_EQ(partition.outcomes().size(), 1u);
	EXPECT_EQ(partition.outcomes()[0].type, OutcomeType::CANCEL_REJECTED);
	EXPECT_EQ(partition.outcomes()[0].reason, reject_reason::UNKNOWN_SYMBOL);
}

// Depth carries no identity, so there is nobody to report a misroute to — the
// counter is the only place it shows up.
TEST(EnginePartition, MisroutedDepthIsCountedButProducesNoOutcome) {
	engine_partition<256> partition(nullptr);
	partition.listing(1);

	ASSERT_TRUE(partition.submit(event::command::add(2, side_t::bid, 100, 10)));
	ASSERT_TRUE(partition.submit(event::command::reduce(2, side_t::ask, 101, 5)));
	EXPECT_EQ(partition.drain(), 2u);

	EXPECT_EQ(partition.misrouted(), 2u);
	EXPECT_TRUE(partition.outcomes().empty());
}

// One partition, several listings, no cross-talk: that is the whole point of the
// engine looking a book up per command instead of owning one.
TEST(EnginePartition, ListingsOnOnePartitionDoNotSeeEachOther) {
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
// same price — the two books never meet.
TEST(EnginePartition, OrdersDoNotCrossBetweenListings) {
	engine_partition<256> partition(nullptr);
	partition.listing(1);
	partition.listing(2);

	ASSERT_TRUE(partition.submit(event::command::add(1, side_t::ask, 100, 10)));
	ASSERT_TRUE(partition.submit(event::command::place(
		{.id = 5, .symbol_id = 2, .side = side_t::bid, .price = 100, .qty = 10})));
	EXPECT_EQ(partition.drain(), 2u);

	EXPECT_TRUE(partition.trades().empty()) << "listings must not cross";
	EXPECT_EQ(partition.book(1)->volume_at_price(100, side_t::ask), 10);
	EXPECT_EQ(partition.book(2)->volume_at_price(100, side_t::bid), 10);
}

// A listing registered after the fact starts working; nothing has to be rebuilt.
TEST(EnginePartition, RegisteringAListingLaterMakesItsCommandsLand) {
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
TEST(Routing, OnlyTheOwningPartitionAcceptsACommand) {
	constexpr std::size_t PARTITIONS = 4;
	const dispatcher route(PARTITIONS);
	std::vector<std::unique_ptr<engine_partition<256> > > partitions;
	for (std::size_t i = 0; i < PARTITIONS; ++i)
		partitions.push_back(std::make_unique<engine_partition<256> >(nullptr));

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
			EXPECT_EQ(partitions[index]->book(SYMBOL)->volume_at_price(
						  100, side_t::bid),
					  10);
		} else {
			EXPECT_EQ(partitions[index]->misrouted(), 1u) << index;
			EXPECT_EQ(partitions[index]->book(SYMBOL), nullptr) << index;
		}
	}
}
