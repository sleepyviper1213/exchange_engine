#include "trading-engine/execution/book_manager.hpp"
#include "trading-engine/execution/dispatcher.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// The dispatcher and the book manager together: a command routed by symbol
// reaches the one partition carrying that listing, and no other.

using namespace exchange::engine;
using namespace exchange::engine::execution;
using namespace exchange;

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

