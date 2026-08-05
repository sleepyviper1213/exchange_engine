#include "market-data/binance/binance_depth.hpp"
#include "market-data/binance/normalise.hpp"
#include "market-data/l2_book.hpp"
#include "market-data/normalised.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

using exchange::side_t;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::l2_book;
using exchange::market_data::sequence_range;
using exchange::market_data::timestamp;

namespace binance = exchange::market_data::binance;

// apply — one normalised diff onto an l2_book, via absolute set_level writes.

namespace {

TEST(ApplyEvent, SetsAbsoluteSizesOnBothSides) {
	l2_book book;
	const depth_event event{{1, 1}, timestamp{}, {{100, 5}}, {{101, 7}}};
	apply(book, event);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 5);
	EXPECT_EQ(book.volume_at_price(101, side_t::ask), 7);
}

TEST(ApplyEvent, ZeroSizeRemovesTheLevel) {
	l2_book book;
	apply(book, depth_event{{1, 1}, timestamp{}, {{100, 5}}, {}});
	ASSERT_EQ(book.depth(side_t::bid), 1u);
	// The diff primitive: a level published at size 0 is a removal.
	apply(book, depth_event{{2, 2}, timestamp{}, {{100, 0}}, {}});
	EXPECT_EQ(book.depth(side_t::bid), 0u);
}

TEST(ApplyEvent, ApplyingTheSameEventTwiceIsIdempotent) {
	l2_book book;
	const depth_event event{{1, 1}, timestamp{}, {{100, 5}, {99, 3}}, {}};
	apply(book, event);
	apply(book, event);
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 5);
}

} // namespace
