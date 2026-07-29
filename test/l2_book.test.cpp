#include "market-data/l2_book.hpp"

#include <gtest/gtest.h>

#include <optional>

using exchange::side;
using exchange::market_data::l2_book;

namespace {

TEST(L2Book, EmptyHasNoBestNorVolume) {
	const l2_book book;
	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_FALSE(book.best_ask().has_value());
	EXPECT_EQ(book.depth(side::bid), 0u);
	EXPECT_EQ(book.depth(side::ask), 0u);
	EXPECT_EQ(book.volume_at_price(100, side::bid), 0);
}

TEST(L2Book, SetLevelInsertsAndReadsBack) {
	l2_book book;
	book.set_level(side::bid, 100, 5);
	EXPECT_EQ(book.volume_at_price(100, side::bid), 5);
	EXPECT_EQ(book.best_bid().value(), 100);
	EXPECT_EQ(book.depth(side::bid), 1u);
	// A price is per-side: the same price on the ask side is independent.
	EXPECT_EQ(book.volume_at_price(100, side::ask), 0);
}

TEST(L2Book, BidsDescendingBestIsHighest) {
	l2_book book;
	book.set_level(side::bid, 100, 1);
	book.set_level(side::bid, 102, 1); // higher -> becomes best
	book.set_level(side::bid, 101, 1); // lands between the two
	EXPECT_EQ(book.best_bid().value(), 102);

	const auto &bids = book.levels(side::bid);
	ASSERT_EQ(bids.size(), 3u);
	EXPECT_EQ(bids[0].price, 102u); // strictly descending, best first
	EXPECT_EQ(bids[1].price, 101u);
	EXPECT_EQ(bids[2].price, 100u);
}

TEST(L2Book, AsksAscendingBestIsLowest) {
	l2_book book;
	book.set_level(side::ask, 102, 1);
	book.set_level(side::ask, 100, 1); // lower -> becomes best
	book.set_level(side::ask, 101, 1);
	EXPECT_EQ(book.best_ask().value(), 100);

	const auto &asks = book.levels(side::ask);
	ASSERT_EQ(asks.size(), 3u);
	EXPECT_EQ(asks[0].price, 100u); // strictly ascending, best first
	EXPECT_EQ(asks[1].price, 101u);
	EXPECT_EQ(asks[2].price, 102u);
}

TEST(L2Book, SetLevelOnExistingPriceOverwritesVolume) {
	l2_book book;
	book.set_level(side::ask, 100, 5);
	book.set_level(side::ask, 100, 9); // absolute, not additive
	EXPECT_EQ(book.volume_at_price(100, side::ask), 9);
	EXPECT_EQ(book.depth(side::ask), 1u);
}

TEST(L2Book, ZeroOrNegativeVolumeRemovesLevel) {
	l2_book book;
	book.set_level(side::bid, 100, 5);
	book.set_level(side::bid, 101, 7);
	book.set_level(side::bid, 100, 0); // remove the lower level
	EXPECT_EQ(book.volume_at_price(100, side::bid), 0);
	EXPECT_EQ(book.depth(side::bid), 1u);
	EXPECT_EQ(book.best_bid().value(), 101);

	book.set_level(side::bid, 101, -3); // <=0 also removes
	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_EQ(book.depth(side::bid), 0u);
}

TEST(L2Book, RemovingAnAbsentPriceIsANoOp) {
	l2_book book;
	book.set_level(side::ask, 100, 5);
	book.set_level(side::ask, 999, 0); // never existed -> no-op
	EXPECT_EQ(book.depth(side::ask), 1u);
	EXPECT_EQ(book.volume_at_price(100, side::ask), 5);
}

TEST(L2Book, ClearEmptiesBothSides) {
	l2_book book;
	book.set_level(side::bid, 100, 5);
	book.set_level(side::ask, 101, 5);
	book.clear();
	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_FALSE(book.best_ask().has_value());
	EXPECT_EQ(book.depth(side::bid), 0u);
	EXPECT_EQ(book.depth(side::ask), 0u);
}

} // namespace
