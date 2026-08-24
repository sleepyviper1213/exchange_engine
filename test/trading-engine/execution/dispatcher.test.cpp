#include "execution/dispatcher.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>

// Which partition owns a listing. Routing is a pure function of the symbol id
// and the partition count, so the properties under test are the ones that let a
// producer, a consumer and a recovery replay each compute it independently and
// still agree.

using namespace exchange::engine;
using namespace exchange::engine::execution;
using namespace exchange;

TEST(Dispatcher, OnePartitionOwnsEverything) {
	const dispatcher route(1);
	EXPECT_EQ(route.partition_count(), 1u);
	for (symbol_id_t symbol = 0; symbol < 100; ++symbol)
		EXPECT_EQ(route.partition_for(symbol), 0u);
}

TEST(Dispatcher, DenseSymbolsDealOutRoundRobin) {
	const dispatcher route(4);
	EXPECT_EQ(route.partition_for(0), 0u);
	EXPECT_EQ(route.partition_for(1), 1u);
	EXPECT_EQ(route.partition_for(2), 2u);
	EXPECT_EQ(route.partition_for(3), 3u);
	EXPECT_EQ(route.partition_for(4), 0u);
}

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

