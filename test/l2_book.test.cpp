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
	// they could have — both naming the same side — is worth pinning down. Both
	// sides now live in one block, which makes an off-by-one in the split
	// between them exactly this kind of failure.
	l2_book book;
	book.set_level(side_t::bid, 100, 5);
	book.set_level(side_t::ask, 101, 7);

	ASSERT_EQ(book.bid_levels().size(), 1u);
	ASSERT_EQ(book.ask_levels().size(), 1u);
	EXPECT_EQ(book.bid_levels().front().price, 100u);
	EXPECT_EQ(book.ask_levels().front().price, 101u);
	EXPECT_NE(book.bid_levels().data(), book.ask_levels().data());
}

TEST(L2Book, SideAccessorsAreEmptyOnAFreshBook) {
	const l2_book book;
	EXPECT_TRUE(book.bid_levels().empty());
	EXPECT_TRUE(book.ask_levels().empty());
}

// --- max_depth: the top-N retention window -----------------------------------
//
// The depth is fixed at construction and the storage never grows, so the cap
// bounds set_level's insert/erase shift and rules out reallocation entirely.
// These cases pin down what that costs the book's contents, because it is a
// lossy view and the loss has to be the predictable kind: the worst levels go,
// the best stay, and the ordering invariant holds.

TEST(L2Book, DefaultConstructedBookUsesTheDefaultDepth) {
	const l2_book book;
	EXPECT_EQ(book.max_depth(), l2_book::DEFAULT_DEPTH);
	EXPECT_GT(book.max_depth(), 0u); // there is no unbounded setting any more
}

TEST(L2Book, CapReportsItsOwnDepth) {
	const l2_book book(64);
	EXPECT_EQ(book.max_depth(), 64u);
}

// The cells live in one block owned for the book's lifetime, so a span handed
// out earlier still names the same memory after the side has been rewritten
// many times over. A vector-backed book could not promise this.
TEST(L2Book, TheCellsNeverMove) {
	l2_book book(8);
	book.set_level(side_t::bid, 100, 1);
	const auto *first = book.bid_levels().data();

	for (exchange::price_t price = 101; price < 140; ++price)
		book.set_level(side_t::bid, price, 1); // far past capacity
	book.load(side_t::bid, {{200, 1}, {199, 1}});
	book.clear();
	book.set_level(side_t::bid, 100, 1);

	EXPECT_EQ(book.bid_levels().data(), first);
}

// --- dropped_levels: the cap's cost, made countable --------------------------

TEST(L2Book, AFreshBookHasDroppedNothing) {
	const l2_book book(4);
	EXPECT_EQ(book.dropped_levels(), 0u);
}

TEST(L2Book, LevelsThatFitAreNotCountedAsDropped) {
	l2_book book(4);
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::bid, 99, 1);
	book.load(side_t::ask, {{200, 1}, {201, 1}});
	EXPECT_EQ(book.dropped_levels(), 0u);
}

TEST(L2Book, APriceOutsideAFullWindowIsCountedAsDropped) {
	l2_book book(2);
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::bid, 99, 1);
	book.set_level(side_t::bid, 98, 1); // worse than both: never stored
	EXPECT_EQ(book.dropped_levels(), 1u);
	EXPECT_EQ(book.depth(side_t::bid), 2u);
}

TEST(L2Book, EvictingTheWorstLevelIsCountedAsDropped) {
	l2_book book(2);
	book.set_level(side_t::bid, 100, 1);
	book.set_level(side_t::bid, 98, 1);
	book.set_level(side_t::bid, 99, 1); // lands inside; 98 is evicted
	EXPECT_EQ(book.dropped_levels(), 1u);
	EXPECT_EQ(book.volume_at_price(98, side_t::bid), 0);
}

TEST(L2Book, LoadCountsTheDepthItCouldNotKeep) {
	l2_book book(2);
	book.load(side_t::bid, {{100, 1}, {99, 1}, {98, 1}, {97, 1}, {96, 1}});
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.dropped_levels(), 3u);
}

// A zero-size level is not depth the window refused — it is not a level at all.
TEST(L2Book, LoadDoesNotCountNonPositiveLevelsAsDropped) {
	l2_book book(4);
	book.load(side_t::bid, {{100, 1}, {99, 0}, {98, -5}, {97, 1}});
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.dropped_levels(), 0u);
}

// Nor is a duplicated price, which was never a distinct level.
TEST(L2Book, LoadDoesNotCountDuplicatePricesAsDropped) {
	l2_book book(4);
	book.load(side_t::bid, {{100, 1}, {100, 2}, {99, 1}});
	EXPECT_EQ(book.depth(side_t::bid), 2u);
	EXPECT_EQ(book.volume_at_price(100, side_t::bid), 1); // first wins
	EXPECT_EQ(book.dropped_levels(), 0u);
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

// --------------------------------------------------------------------------
// crossed() — the sanity check sequence numbers cannot provide
// --------------------------------------------------------------------------

TEST(L2Book, AnEmptyOrOneSidedBookIsNotCrossed) {
	l2_book book;
	EXPECT_FALSE(book.is_crossed());
	book.set_level(side_t::bid, 100, 5);
	EXPECT_FALSE(book.is_crossed()); // nothing on the other side to cross with
	book.clear();
	book.set_level(side_t::ask, 100, 5);
	EXPECT_FALSE(book.is_crossed());
}

TEST(L2Book, AProperlySpreadBookIsNotCrossed) {
	l2_book book;
	book.set_level(side_t::bid, 100, 5);
	book.set_level(side_t::ask, 101, 5);
	EXPECT_FALSE(book.is_crossed());
}

TEST(L2Book, ABidAboveTheBestAskIsCrossed) {
	l2_book book;
	book.set_level(side_t::ask, 100, 5);
	book.set_level(side_t::bid, 105, 5);
	EXPECT_TRUE(book.is_crossed());
}

// Locked counts as crossed: on a continuous-matching venue a bid and ask at the
// same price should have traded, so a book that reports one is evidence the
// replica is wrong. @see reconstructor_options::resync_on_cross
TEST(L2Book, ALockedBookIsReportedAsCrossed) {
	l2_book book;
	book.set_level(side_t::bid, 100, 5);
	book.set_level(side_t::ask, 100, 5);
	EXPECT_TRUE(book.is_crossed());
}

// Only the touch matters — depth behind it may overlap the other side freely.
TEST(L2Book, OnlyTheTouchDecidesWhetherTheBookIsCrossed) {
	l2_book book;
	book.load(side_t::bid, {{100, 1}, {99, 1}, {98, 1}});
	book.load(side_t::ask, {{101, 1}, {102, 1}});
	ASSERT_FALSE(book.is_crossed());

	// Withdraw the best ask so the next one is still above the bid: fine.
	book.set_level(side_t::ask, 101, 0);
	EXPECT_FALSE(book.is_crossed());

	// Withdraw the best bid and add one through the ask: crossed.
	book.set_level(side_t::bid, 103, 1);
	EXPECT_TRUE(book.is_crossed());
}

TEST(L2Book, ClearingResolvesACross) {
	l2_book book;
	book.set_level(side_t::ask, 100, 5);
	book.set_level(side_t::bid, 105, 5);
	ASSERT_TRUE(book.is_crossed());
	book.clear();
	EXPECT_FALSE(book.is_crossed());
}

} // namespace
