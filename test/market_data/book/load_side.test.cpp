#include "market_data/l2_book.hpp"
#include "market_data/normalised.hpp"

#include <gtest/gtest.h>

#include <optional>

using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::l2_book;
using exchange::market_data::timestamp;

// l2_book::load - a side installed wholesale, in any order the venue sent.

namespace {

TEST(LoadSide, LeavesTheOtherSideAlone) {
	l2_book book;
	book.set_level(side_t::ask, 200, 4);
	book.load(side_t::bid, {{100, 1}, {101, 2}});
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.volume_at_price(200, side_t::ask), 4);
}

TEST(LoadSide, LeavesTheSideUsableBySetLevel) {
	l2_book book;
	book.load(side_t::bid, {{99, 1}, {101, 2}, {100, 3}});
	// The loaded side must satisfy the sorted invariant set_level assumes.
	book.set_level(side_t::bid, 100, 8); // existing price -> overwrite
	book.set_level(side_t::bid, 102, 4); // new best -> inserted at the front
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 8);
	EXPECT_EQ(book.best_bid(), std::optional<exchange::price_t>{102});
	EXPECT_EQ(book.depth(side_t::bid), 4u);
}

} // namespace
