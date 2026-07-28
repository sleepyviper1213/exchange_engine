#include "market-data/l2_book.hpp"

#include <gtest/gtest.h>

#include <optional>

using exchange::Side;
using exchange::market_data::l2_book;

namespace {

TEST(L2Book, EmptyHasNoBestNorVolume) {
	const l2_book book;
	EXPECT_EQ(book.best_bid(), std::nullopt);
	EXPECT_EQ(book.best_ask(), std::nullopt);
	EXPECT_EQ(book.depth(Side::BID), 0u);
	EXPECT_EQ(book.depth(Side::ASK), 0u);
	EXPECT_EQ(book.volume_at_price(100, Side::BID), 0);
}

TEST(L2Book, SetLevelInsertsAndReadsBack) {
	l2_book book;
	book.set_level(Side::BID, 100, 5);
	EXPECT_EQ(book.volume_at_price(100, Side::BID), 5);
	EXPECT_EQ(book.best_bid(), std::optional<exchange::Price>{100});
	EXPECT_EQ(book.depth(Side::BID), 1u);
	// A price is per-side: the same price on the ask side is independent.
	EXPECT_EQ(book.volume_at_price(100, Side::ASK), 0);
}

TEST(L2Book, BidsDescendingBestIsHighest) {
	l2_book book;
	book.set_level(Side::BID, 100, 1);
	book.set_level(Side::BID, 102, 1); // higher -> becomes best
	book.set_level(Side::BID, 101, 1); // lands between the two
	EXPECT_EQ(book.best_bid(), std::optional<exchange::Price>{102});

	const auto &bids = book.levels(Side::BID);
	ASSERT_EQ(bids.size(), 3u);
	EXPECT_EQ(bids[0].price, 102u); // strictly descending, best first
	EXPECT_EQ(bids[1].price, 101u);
	EXPECT_EQ(bids[2].price, 100u);
}

TEST(L2Book, AsksAscendingBestIsLowest) {
	l2_book book;
	book.set_level(Side::ASK, 102, 1);
	book.set_level(Side::ASK, 100, 1); // lower -> becomes best
	book.set_level(Side::ASK, 101, 1);
	EXPECT_EQ(book.best_ask(), std::optional<exchange::Price>{100});

	const auto &asks = book.levels(Side::ASK);
	ASSERT_EQ(asks.size(), 3u);
	EXPECT_EQ(asks[0].price, 100u); // strictly ascending, best first
	EXPECT_EQ(asks[1].price, 101u);
	EXPECT_EQ(asks[2].price, 102u);
}

TEST(L2Book, SetLevelOnExistingPriceOverwritesVolume) {
	l2_book book;
	book.set_level(Side::ASK, 100, 5);
	book.set_level(Side::ASK, 100, 9); // absolute, not additive
	EXPECT_EQ(book.volume_at_price(100, Side::ASK), 9);
	EXPECT_EQ(book.depth(Side::ASK), 1u);
}

TEST(L2Book, ZeroOrNegativeVolumeRemovesLevel) {
	l2_book book;
	book.set_level(Side::BID, 100, 5);
	book.set_level(Side::BID, 101, 7);
	book.set_level(Side::BID, 100, 0); // remove the lower level
	EXPECT_EQ(book.volume_at_price(100, Side::BID), 0);
	EXPECT_EQ(book.depth(Side::BID), 1u);
	EXPECT_EQ(book.best_bid(), std::optional<exchange::Price>{101});

	book.set_level(Side::BID, 101, -3); // <=0 also removes
	EXPECT_EQ(book.best_bid(), std::nullopt);
	EXPECT_EQ(book.depth(Side::BID), 0u);
}

TEST(L2Book, RemovingAnAbsentPriceIsANoOp) {
	l2_book book;
	book.set_level(Side::ASK, 100, 5);
	book.set_level(Side::ASK, 999, 0); // never existed -> no-op
	EXPECT_EQ(book.depth(Side::ASK), 1u);
	EXPECT_EQ(book.volume_at_price(100, Side::ASK), 5);
}

TEST(L2Book, ClearEmptiesBothSides) {
	l2_book book;
	book.set_level(Side::BID, 100, 5);
	book.set_level(Side::ASK, 101, 5);
	book.clear();
	EXPECT_EQ(book.best_bid(), std::nullopt);
	EXPECT_EQ(book.best_ask(), std::nullopt);
	EXPECT_EQ(book.depth(Side::BID), 0u);
	EXPECT_EQ(book.depth(Side::ASK), 0u);
}

} // namespace
