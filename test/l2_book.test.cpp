#include "market-data/l2_book.hpp"

#include <gtest/gtest.h>

#include <optional>

using exchange::side_t;
using exchange::market_data::l2_book;

namespace {

TEST(L2Book, EmptyHasNoBestNorVolume) {
	const l2_book book;
	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_FALSE(book.best_ask().has_value());
	EXPECT_EQ(book.depth(side_t::bid), 0u);
	EXPECT_EQ(book.depth(side_t::ask), 0u);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 0);
}

TEST(L2Book, SetLevelInsertsAndReadsBack) {
	l2_book book;
	book.set_level(side_t::bid, 100, 5);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 5);
	EXPECT_EQ(book.best_bid().value(), 100);
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	// A price is per-side: the same price on the ask side is independent.
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 0);
}

TEST(L2Book, BidsDescendingBestIsHighest) {
	l2_book book;
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::bid, 102, 1); // higher -> becomes best
	book.set_level(side_t::bid, 101, 1); // lands between the two
	EXPECT_EQ(book.best_bid().value(), 102);

	const auto &bids = book.bid_levels();
	ASSERT_EQ(bids.size(), 3u);
	EXPECT_EQ(bids[0].price, 102u); // strictly descending, best first
	EXPECT_EQ(bids[1].price, 101u);
	EXPECT_EQ(bids[2].price, 100u);
}

TEST(L2Book, AsksAscendingBestIsLowest) {
	l2_book book;
	book.set_level(side_t::ask, 102, 1);
	book.set_level(side_t::ask, 100, 1); // lower -> becomes best
	book.set_level(side_t::ask, 101, 1);
	EXPECT_EQ(book.best_ask().value(), 100);

	const auto &asks = book.ask_levels();
	ASSERT_EQ(asks.size(), 3u);
	EXPECT_EQ(asks[0].price, 100u); // strictly ascending, best first
	EXPECT_EQ(asks[1].price, 101u);
	EXPECT_EQ(asks[2].price, 102u);
}

TEST(L2Book, SetLevelOnExistingPriceOverwritesVolume) {
	l2_book book;
	book.set_level(side_t::ask, 100, 5);
	book.set_level(side_t::ask, 100, 9); // absolute, not additive
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 9);
	EXPECT_EQ(book.depth(side_t::ask), 1u);
}

TEST(L2Book, ZeroOrNegativeVolumeRemovesLevel) {
	l2_book book;
	book.set_level(side_t::bid, 100, 5);
	book.set_level(side_t::bid, 101, 7);
	book.set_level(side_t::bid, 100, 0); // remove the lower level
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 0);
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	EXPECT_EQ(book.best_bid().value(), 101);

	book.set_level(side_t::bid, 101, -3); // <=0 also removes
	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_EQ(book.depth(side_t::bid), 0u);
}

TEST(L2Book, RemovingAnAbsentPriceIsANoOp) {
	l2_book book;
	book.set_level(side_t::ask, 100, 5);
	book.set_level(side_t::ask, 999, 0); // never existed -> no-op
	EXPECT_EQ(book.depth(side_t::ask), 1u);
	EXPECT_EQ(book.volume_at_price(100, side_t::ask), 5);
}

TEST(L2Book, ClearEmptiesBothSides) {
	l2_book book;
	book.set_level(side_t::bid, 100, 5);
	book.set_level(side_t::ask, 101, 5);
	book.clear();
	EXPECT_FALSE(book.best_bid().has_value());
	EXPECT_FALSE(book.best_ask().has_value());
	EXPECT_EQ(book.depth(side_t::bid), 0u);
	EXPECT_EQ(book.depth(side_t::ask), 0u);
}

TEST(L2Book, SideAccessorsReturnDistinctSides) {
	// The two accessors are separate one-line member reads, so the cheap failure
	// they could have — both naming the same vector — is worth pinning down.
	l2_book book;
	book.set_level(side_t::bid, 100, 5);
	book.set_level(side_t::ask, 101, 7);

	ASSERT_EQ(book.bid_levels().size(), 1u);
	ASSERT_EQ(book.ask_levels().size(), 1u);
	EXPECT_EQ(book.bid_levels().front().price, 100u);
	EXPECT_EQ(book.ask_levels().front().price, 101u);
	EXPECT_NE(&book.bid_levels(), &book.ask_levels());
}

TEST(L2Book, SideAccessorsAreEmptyOnAFreshBook) {
	const l2_book book;
	EXPECT_TRUE(book.bid_levels().empty());
	EXPECT_TRUE(book.ask_levels().empty());
}

// --- max_depth: the top-N retention window -----------------------------------
//
// The cap bounds set_level's insert/erase shift, which is what a per-update
// latency budget needs. These cases pin down what it does to the book's
// contents, because it is a lossy view and the loss has to be the predictable
// kind: the worst levels go, the best stay, and the ordering invariant holds.

TEST(L2Book, DefaultConstructedBookIsUncapped) {
	const l2_book book;
	EXPECT_EQ(book.max_depth(), l2_book::UNBOUNDED_DEPTH);
}

TEST(L2Book, CapReportsItsOwnDepth) {
	const l2_book book(64);
	EXPECT_EQ(book.max_depth(), 64u);
}

TEST(L2Book, LoadTruncatesToCapKeepingTheBestLevels) {
	l2_book book(3);
	// Deliberately unsorted, and one non-positive size, so truncation is proven
	// to happen after load() establishes best-first order rather than before.
	book.load(side_t::bid, {{100, 1}, {104, 1}, {101, 1}, {103, 0}, {102, 1}});

	ASSERT_EQ(book.depth(side_t::bid), 3u);
	const auto &bids = book.bid_levels();
	EXPECT_EQ(bids[0].price, 104u); // the three best bids survive,
	EXPECT_EQ(bids[1].price, 102u); // 103 having been dropped as size 0,
	EXPECT_EQ(bids[2].price, 101u);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 0); // and 100 is gone.
}

TEST(L2Book, InsertBeyondAFullWindowIsDropped) {
	l2_book book(2);
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::bid, 99, 1);
	// 98 is worse than every retained level, so it never enters the window.
	book.set_level(side_t::bid, 98, 1);

	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.volume_at_price(98, side_t::bid), 0);
	EXPECT_EQ(book.best_bid().value(), 100);
}

TEST(L2Book, InsertInsideAFullWindowEvictsTheWorstLevel) {
	l2_book book(2);
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::bid, 98, 1);
	book.set_level(side_t::bid, 99, 2); // lands between; 98 is evicted

	ASSERT_EQ(book.depth(side_t::bid), 2u);
	const auto &bids = book.bid_levels();
	EXPECT_EQ(bids[0].price, 100u);
	EXPECT_EQ(bids[1].price, 99u);
	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 2);
	EXPECT_EQ(book.volume_at_price(98, side_t::bid), 0);
}

TEST(L2Book, NewBestPriceEvictsTheWorstAndStaysSorted) {
	l2_book book(3);
	book.load(side_t::bid, {{100, 1}, {99, 1}, {98, 1}});
	book.set_level(side_t::bid, 101, 5); // new touch; 98 falls out of the window

	ASSERT_EQ(book.depth(side_t::bid), 3u);
	const auto &bids = book.bid_levels();
	EXPECT_EQ(bids[0].price, 101u);
	EXPECT_EQ(bids[1].price, 100u);
	EXPECT_EQ(bids[2].price, 99u);
	EXPECT_EQ(book.best_bid().value(), 101);
}

TEST(L2Book, AskWindowRetainsTheLowestPrices) {
	l2_book book(2);
	book.set_level(side_t::ask, 100, 1);
	book.set_level(side_t::ask, 101, 1);
	book.set_level(side_t::ask, 102, 1); // worse than both -> dropped
	book.set_level(side_t::ask, 99, 1);  // better than both -> 101 evicted

	ASSERT_EQ(book.depth(side_t::ask), 2u);
	const auto &asks = book.ask_levels();
	EXPECT_EQ(asks[0].price, 99u);
	EXPECT_EQ(asks[1].price, 100u);
	EXPECT_EQ(book.best_ask().value(), 99);
}

TEST(L2Book, OverwriteAndEraseAreUnaffectedByTheCap) {
	l2_book book(2);
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::bid, 99, 1);

	book.set_level(side_t::bid, 99, 7); // a full window never blocks an overwrite
	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 7);
	EXPECT_EQ(book.depth(side_t::bid), 2u);

	book.set_level(side_t::bid, 100, 0); // erase frees a slot
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	book.set_level(side_t::bid, 98, 3);  // which a worse price can now take
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.volume_at_price(98, side_t::bid), 3);
}

TEST(L2Book, CapIsPerSide) {
	l2_book book(1);
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::ask, 101, 1);
	EXPECT_EQ(book.depth(side_t::bid), 1u);
	EXPECT_EQ(book.depth(side_t::ask), 1u);
}

TEST(L2Book, EvictedDepthDoesNotReturnWhenTheWindowReopens) {
	// The honest statement of what a capped book loses. An L2 diff feed only
	// reports prices whose size CHANGED, so a level pushed out of the window is
	// gone until the venue happens to send it again — the book cannot recover it
	// by itself. Anything needing full published depth must stay uncapped.
	l2_book book(2);
	book.load(side_t::bid, {{100, 1}, {99, 1}});
	book.set_level(side_t::bid, 101, 1); // 99 evicted
	book.set_level(side_t::bid, 101, 0); // touch withdrawn, window has room

	EXPECT_EQ(book.depth(side_t::bid), 1u);
	EXPECT_EQ(book.volume_at_price(99, side_t::bid), 0); // not resurrected
}

} // namespace
