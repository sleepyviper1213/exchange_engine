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

// reset — replace a book wholesale with a snapshot.

namespace {

TEST(ResetBook, InstallsBothSidesSortedBestFirst) {
	l2_book book;
	// Deliberately unsorted: a normalised snapshot makes no ordering promise,
	// because venues disagree about it.
	reset(book,
		  book_snapshot{42, timestamp{}, {{99, 1}, {101, 2}, {100, 3}},
						{{105, 1}, {103, 2}, {104, 3}}});

	const auto &bids = book.bid_levels();
	ASSERT_EQ(bids.size(), 3u);
	EXPECT_EQ(bids[0].price, 101u); // bids descending
	EXPECT_EQ(bids[1].price, 100u);
	EXPECT_EQ(bids[2].price, 99u);

	const auto &asks = book.ask_levels();
	ASSERT_EQ(asks.size(), 3u);
	EXPECT_EQ(asks[0].price, 103u); // asks ascending
	EXPECT_EQ(asks[1].price, 104u);
	EXPECT_EQ(asks[2].price, 105u);
}

TEST(ResetBook, ReplacesEverythingThatWasThereBefore) {
	l2_book book;
	book.set_level(side_t::bid, 50, 9);
	book.set_level(side_t::ask, 60, 9);

	reset(book, book_snapshot{1, timestamp{}, {{100, 1}}, {}});
	EXPECT_EQ(book.volume_at_price(50, side_t::bid), 0); // gone, not merged
	EXPECT_EQ(book.depth(side_t::ask), 0u);              // an empty side_t clears
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 1);
}

TEST(ResetBook, DropsNonPositiveSizes) {
	l2_book book;
	reset(book, book_snapshot{1, timestamp{}, {{100, 5}, {99, 0}}, {}});
	// A zero-size level is the same state as an absent one; it must not become
	// a cell the binary search then has to step over.
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 0);
}

TEST(ResetBook, KeepsOnePricePerSide) {
	l2_book book;
	reset(book, book_snapshot{1, timestamp{}, {{100, 5}, {100, 7}}, {}});
	// A duplicate price would break set_level's binary search; the first wins.
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 5);
}

} // namespace
