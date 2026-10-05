#include "market_data/book_snapshot.hpp"
#include "market_data/depth_event.hpp"
#include "market_data/l2_book.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>

using namespace exchange;
using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::l2_book;
using price_level = exchange::market_data::l2_book::price_level;
using exchange::market_data::timestamp;

// l2_book::load - a side installed wholesale, in any order the venue sent.

namespace {

TEST(LoadSide, LeavesTheOtherSideAlone) {
	l2_book book;
	book.set_level(side_t::ask, at_scaled(200), 4 * units::scaled_size);
	const auto bids =
		std::to_array<price_level>({{at_scaled(100), 1 * units::scaled_size},
									{at_scaled(101), 2 * units::scaled_size}});
	book.load(side_t::bid, bids);
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.volume_at_price(at_scaled(200), side_t::ask),
			  4 * units::scaled_size);
}

TEST(LoadSide, LeavesTheSideUsableBySetLevel) {
	l2_book book;
	const auto bids =
		std::to_array<price_level>({{at_scaled(99), 1 * units::scaled_size},
									{at_scaled(101), 2 * units::scaled_size},
									{at_scaled(100), 3 * units::scaled_size}});
	book.load(side_t::bid, bids);
	// The loaded side must satisfy the sorted invariant set_level assumes.
	book.set_level(side_t::bid,
				   at_scaled(100),
				   8 * units::scaled_size); // existing price -> overwrite
	book.set_level(side_t::bid,
				   at_scaled(102),
				   4 * units::scaled_size); // new best -> inserted at the front
	EXPECT_EQ(book.volume_at_price(at_scaled(100), side_t::bid),
			  8 * units::scaled_size);
	EXPECT_EQ(book.best_bid().value(), at_scaled(102));
	EXPECT_EQ(book.depth(side_t::bid), 4u);
}

} // namespace
