#include "order_book/cached_optimised_order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <vector>

using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::experimental::cached_optimised_order_book;

namespace {

/// The depth most cases use: deep enough that nothing is evicted, so a case
/// that is not about the capacity never trips over it.
using book = cached_optimised_order_book<8>;

/// Prices on a side, best first. Templated because the capacity cases use
/// books of their own depth.
template <std::size_t N>
std::vector<price_t> prices(const cached_optimised_order_book<N> &b,
							side_t side) {
	std::vector<price_t> out;
	for (const auto &level : b.levels_for(side)) out.push_back(level.price);
	return out;
}

// --------------------------------------------------------------------------
// Empty book
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, EmptyHasNoTouchNoDepthNoVolume) {
	const book b;
	EXPECT_FALSE(b.best_bid().has_value());
	EXPECT_FALSE(b.best_ask().has_value());
	EXPECT_FALSE(b.spread().has_value());
	EXPECT_FALSE(b.is_crossed());
	EXPECT_TRUE(b.is_empty());
	EXPECT_EQ(b.size(), 0u);
	EXPECT_EQ(b.depth(side_t::bid), 0u);
	EXPECT_EQ(b.depth(side_t::ask), 0u);
	EXPECT_EQ(b.volume_at_price(100, side_t::bid), 0);
	EXPECT_EQ(b.dropped_levels(), 0u);
	EXPECT_EQ(b.update_count(), 0u);
}

TEST(CachedOptimisedOrderBook, MaxDepthIsTheTemplateArgumentAndIsPerSide) {
	EXPECT_EQ(book::max_depth(), 8u);
	EXPECT_EQ(cached_optimised_order_book<1>::max_depth(), 1u);

	book b;
	for (price_t p = 100; p < 108; ++p) b.update_level(side_t::bid, p, 1);
	for (price_t p = 200; p < 208; ++p) b.update_level(side_t::ask, p, 1);
	// Both sides get max_depth() levels, so the book holds 2 * max_depth().
	EXPECT_EQ(b.depth(side_t::bid), 8u);
	EXPECT_EQ(b.depth(side_t::ask), 8u);
	EXPECT_EQ(b.size(), 16u);
	EXPECT_EQ(b.dropped_levels(), 0u);
}

// --------------------------------------------------------------------------
// update_level: insert, overwrite, erase
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, InsertCreatesLevelAndReadsBack) {
	book b;
	b.update_level(side_t::bid, 100, 5);

	EXPECT_EQ(b.volume_at_price(100, side_t::bid), 5);
	EXPECT_EQ(b.best_bid().value(), 100u);
	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_FALSE(b.is_empty());
	// A price is per-side: the same price on the ask side is independent.
	EXPECT_EQ(b.volume_at_price(100, side_t::ask), 0);
	EXPECT_EQ(b.depth(side_t::ask), 0u);
}

TEST(CachedOptimisedOrderBook, BidsDescendingBestIsHighest) {
	book b;
	b.update_level(side_t::bid, 100, 1);
	b.update_level(side_t::bid, 102, 1); // higher -> becomes best
	b.update_level(side_t::bid, 101, 1); // lands between the two

	EXPECT_EQ(b.best_bid().value(), 102u);
	EXPECT_EQ(prices(b, side_t::bid), (std::vector<price_t>{102, 101, 100}));
}

TEST(CachedOptimisedOrderBook, AsksAscendingBestIsLowest) {
	book b;
	b.update_level(side_t::ask, 102, 1);
	b.update_level(side_t::ask, 100, 1); // lower -> becomes best
	b.update_level(side_t::ask, 101, 1);

	EXPECT_EQ(b.best_ask().value(), 100u);
	EXPECT_EQ(prices(b, side_t::ask), (std::vector<price_t>{100, 101, 102}));
}

TEST(CachedOptimisedOrderBook, UpdateOnExistingPriceOverwritesVolume) {
	book b;
	b.update_level(side_t::bid, 100, 5);
	b.update_level(side_t::bid, 100, 9);

	// Absolute size, not a delta, and no second level at the same price.
	EXPECT_EQ(b.volume_at_price(100, side_t::bid), 9);
	EXPECT_EQ(b.depth(side_t::bid), 1u);
}

TEST(CachedOptimisedOrderBook, ZeroQuantityRemovesTheLevel) {
	book b;
	b.update_level(side_t::bid, 101, 5);
	b.update_level(side_t::bid, 100, 5);
	b.update_level(side_t::bid, 101, 0);

	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.volume_at_price(101, side_t::bid), 0);
	EXPECT_EQ(b.best_bid().value(), 100u); // the touch moved down
}

TEST(CachedOptimisedOrderBook, NegativeQuantityRemovesTheLevel) {
	book b;
	b.update_level(side_t::ask, 100, 5);
	b.update_level(side_t::ask, 100, -3);

	EXPECT_EQ(b.depth(side_t::ask), 0u);
	EXPECT_FALSE(b.best_ask().has_value());
}

TEST(CachedOptimisedOrderBook, RemovingAnAbsentPriceIsANoOp) {
	book b;
	b.update_level(side_t::bid, 100, 5);
	b.update_level(side_t::bid, 999, 0); // never rested here

	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.best_bid().value(), 100u);
	EXPECT_EQ(b.dropped_levels(), 0u); // a no-op is not a dropped level
}

TEST(CachedOptimisedOrderBook, EraseFromTheMiddleKeepsTheSideSorted) {
	book b;
	for (price_t p : {100u, 101u, 102u, 103u}) b.update_level(side_t::bid, p, 1);
	b.update_level(side_t::bid, 102, 0);

	EXPECT_EQ(prices(b, side_t::bid), (std::vector<price_t>{103, 101, 100}));
	EXPECT_EQ(b.volume_at_price(101, side_t::bid), 1);
	EXPECT_EQ(b.volume_at_price(103, side_t::bid), 1);
}

TEST(CachedOptimisedOrderBook, InsertIntoTheMiddleKeepsTheSideSorted) {
	book b;
	for (price_t p : {100u, 102u, 104u}) b.update_level(side_t::ask, p, 1);
	b.update_level(side_t::ask, 103, 7);

	EXPECT_EQ(prices(b, side_t::ask),
			  (std::vector<price_t>{100, 102, 103, 104}));
	EXPECT_EQ(b.volume_at_price(103, side_t::ask), 7);
	// The shift must carry the neighbours' volumes with them, not just prices.
	EXPECT_EQ(b.volume_at_price(104, side_t::ask), 1);
}

TEST(CachedOptimisedOrderBook, ClearDropsBothSidesAndTheTouch) {
	book b;
	b.update_level(side_t::bid, 100, 5);
	b.update_level(side_t::ask, 101, 5);
	b.clear();

	EXPECT_TRUE(b.is_empty());
	EXPECT_FALSE(b.best_bid().has_value());
	EXPECT_FALSE(b.best_ask().has_value());
	EXPECT_FALSE(b.spread().has_value());

	// Storage is reusable afterwards, with no stale level showing through.
	b.update_level(side_t::bid, 50, 2);
	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.best_bid().value(), 50u);
	EXPECT_EQ(b.volume_at_price(100, side_t::bid), 0);
}

// --------------------------------------------------------------------------
// The cached touch: spread and crossing
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, SpreadIsBestAskMinusBestBid) {
	book b;
	b.update_level(side_t::bid, 100, 1);
	EXPECT_FALSE(b.spread().has_value()); // one side is not a spread

	b.update_level(side_t::ask, 105, 1);
	EXPECT_EQ(b.spread().value(), 5u);
	EXPECT_FALSE(b.is_crossed());

	// The touch is cached, so it has to keep up with a better price arriving.
	b.update_level(side_t::bid, 103, 1);
	EXPECT_EQ(b.spread().value(), 2u);

	// ...and with the best level being removed.
	b.update_level(side_t::bid, 103, 0);
	EXPECT_EQ(b.spread().value(), 5u);
}

TEST(CachedOptimisedOrderBook, CrossedAndLockedBooksReportNoSpread) {
	book b;
	b.update_level(side_t::bid, 105, 1);
	b.update_level(side_t::ask, 100, 1);

	EXPECT_TRUE(b.is_crossed());
	// price_t is unsigned: a crossed book has no representable spread, and
	// nullopt is the answer rather than a wrapped one.
	EXPECT_FALSE(b.spread().has_value());

	book locked;
	locked.update_level(side_t::bid, 100, 1);
	locked.update_level(side_t::ask, 100, 1);
	EXPECT_TRUE(locked.is_crossed());
	EXPECT_FALSE(locked.spread().has_value());
}

TEST(CachedOptimisedOrderBook, OneSidedBookIsNeverCrossed) {
	book b;
	b.update_level(side_t::bid, 100, 1);
	EXPECT_FALSE(b.is_crossed());

	b.update_level(side_t::bid, 100, 0);
	b.update_level(side_t::ask, 100, 1);
	EXPECT_FALSE(b.is_crossed());
}

// --------------------------------------------------------------------------
// Fixed capacity
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, FullSideRefusesAPriceWorseThanEveryLevel) {
	cached_optimised_order_book<3> b;
	for (price_t p : {105u, 104u, 103u}) b.update_level(side_t::bid, p, 1);

	b.update_level(side_t::bid, 100, 9); // worse than all three

	EXPECT_EQ(b.depth(side_t::bid), 3u);
	EXPECT_EQ(prices(b, side_t::bid), (std::vector<price_t>{105, 104, 103}));
	EXPECT_EQ(b.volume_at_price(100, side_t::bid), 0);
	EXPECT_EQ(b.dropped_levels(), 1u);
}

TEST(CachedOptimisedOrderBook, FullSideEvictsTheWorstLevelForABetterPrice) {
	cached_optimised_order_book<3> b;
	for (price_t p : {105u, 104u, 103u}) b.update_level(side_t::bid, p, 1);

	b.update_level(side_t::bid, 106, 9); // better than all three

	EXPECT_EQ(b.depth(side_t::bid), 3u);
	EXPECT_EQ(prices(b, side_t::bid), (std::vector<price_t>{106, 105, 104}));
	EXPECT_EQ(b.best_bid().value(), 106u);
	EXPECT_EQ(b.volume_at_price(103, side_t::bid), 0); // 103 was evicted
	EXPECT_EQ(b.dropped_levels(), 1u);
}

TEST(CachedOptimisedOrderBook, EvictionOnAFullSideNeverDropsTheTouch) {
	// The whole point of the top-N window: whatever is discarded, the best
	// price is retained, so a cross detected here is a real one.
	cached_optimised_order_book<2> b;
	b.update_level(side_t::ask, 100, 1);
	b.update_level(side_t::ask, 101, 1);
	b.update_level(side_t::ask, 99, 1); // best ask, side already full

	EXPECT_EQ(b.best_ask().value(), 99u);
	EXPECT_EQ(prices(b, side_t::ask), (std::vector<price_t>{99, 100}));
	EXPECT_EQ(b.dropped_levels(), 1u);
}

TEST(CachedOptimisedOrderBook, OverwritingOnAFullSideDropsNothing) {
	cached_optimised_order_book<3> b;
	for (price_t p : {105u, 104u, 103u}) b.update_level(side_t::bid, p, 1);

	b.update_level(side_t::bid, 104, 42); // already resting: no insert

	EXPECT_EQ(b.volume_at_price(104, side_t::bid), 42);
	EXPECT_EQ(b.depth(side_t::bid), 3u);
	EXPECT_EQ(b.dropped_levels(), 0u);
}

TEST(CachedOptimisedOrderBook, CapacityIsPerSideNotShared) {
	cached_optimised_order_book<2> b;
	b.update_level(side_t::bid, 100, 1);
	b.update_level(side_t::bid, 99, 1);
	b.update_level(side_t::ask, 101, 1);
	b.update_level(side_t::ask, 102, 1);

	// A full bid side must not refuse anything on the ask side.
	EXPECT_EQ(b.depth(side_t::bid), 2u);
	EXPECT_EQ(b.depth(side_t::ask), 2u);
	EXPECT_EQ(b.dropped_levels(), 0u);
}

TEST(CachedOptimisedOrderBook, DepthOfOneStillTracksTheBestPrice) {
	cached_optimised_order_book<1> b;
	b.update_level(side_t::bid, 100, 1);
	EXPECT_EQ(b.best_bid().value(), 100u);

	b.update_level(side_t::bid, 99, 1); // worse: refused
	EXPECT_EQ(b.best_bid().value(), 100u);
	EXPECT_EQ(b.depth(side_t::bid), 1u);

	b.update_level(side_t::bid, 101, 1); // better: evicts 100
	EXPECT_EQ(b.best_bid().value(), 101u);
	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.dropped_levels(), 2u);

	b.update_level(side_t::bid, 101, 0);
	EXPECT_FALSE(b.best_bid().has_value());
}

// --------------------------------------------------------------------------
// Per-level running statistics
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, LevelStatisticsAccumulateOverWrites) {
	book b;
	b.update_level(side_t::bid, 100, 10);
	b.update_level(side_t::bid, 100, 20);
	b.update_level(side_t::bid, 100, 30);

	const auto level = b.level_at_price(100, side_t::bid);
	ASSERT_TRUE(level.has_value());
	EXPECT_EQ(level->price, 100u);
	EXPECT_EQ(level->volume, 30); // the last absolute size
	EXPECT_EQ(level->count, 3u);  // writes landed here, including the insert
	EXPECT_EQ(level->total_volume, 60u);
	EXPECT_EQ(level->avg_order_size, 20u);
}

TEST(CachedOptimisedOrderBook, LevelStatisticsRestartWhenALevelComesBack) {
	book b;
	b.update_level(side_t::bid, 100, 10);
	b.update_level(side_t::bid, 100, 0); // level leaves the book
	b.update_level(side_t::bid, 100, 4); // and returns

	const auto level = b.level_at_price(100, side_t::bid);
	ASSERT_TRUE(level.has_value());
	EXPECT_EQ(level->count, 1u);
	EXPECT_EQ(level->total_volume, 4u);
}

TEST(CachedOptimisedOrderBook, LevelTimestampMarksTheLastWriteToThatLevel) {
	book b;
	b.update_level(side_t::bid, 100, 1); // update 1
	b.update_level(side_t::bid, 101, 1); // update 2
	b.update_level(side_t::bid, 100, 2); // update 3, back to the first level

	EXPECT_EQ(b.update_count(), 3u);
	EXPECT_EQ(b.level_at_price(100, side_t::bid)->timestamp, 3u);
	EXPECT_EQ(b.level_at_price(101, side_t::bid)->timestamp, 2u);
}

TEST(CachedOptimisedOrderBook, LevelAtPriceIsNulloptWhenNothingRests) {
	book b;
	b.update_level(side_t::bid, 100, 1);
	EXPECT_FALSE(b.level_at_price(101, side_t::bid).has_value());
	EXPECT_FALSE(b.level_at_price(100, side_t::ask).has_value());
}

// --------------------------------------------------------------------------
// Differential test against a std::map model
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, MatchesAnOrderedMapModelOverRandomUpdates) {
	// The sorted-array paths (binary search, insert shift, erase shift) are
	// where an off-by-one hides, and a hand-written case only covers the shapes
	// someone thought of. This drives the book and a std::map through the same
	// stream and compares both sides in full after every update.
	//
	// The window is sized so nothing is ever evicted -- once a level is dropped
	// the book legitimately diverges from a model that kept everything, and
	// this case is about the shifts, not the capacity. dropped_levels() is
	// asserted to be 0 so a change that starts evicting fails here loudly
	// rather than quietly weakening the comparison.
	constexpr std::size_t depth = 64;
	constexpr int distinct      = 20; // per side, well under `depth`

	cached_optimised_order_book<depth> b;
	std::map<price_t, quantity_t, std::greater<>> bids; // descending
	std::map<price_t, quantity_t, std::less<>> asks;    // ascending

	std::mt19937 rng(20260806); // fixed seed: a failure is reproducible

	for (int i = 0; i < 4000; ++i) {
		const auto side = (rng() % 2) == 0 ? side_t::bid : side_t::ask;
		const auto price =
			static_cast<price_t>(1000 + (rng() % static_cast<unsigned>(distinct)));
		// A third of updates remove, so erase paths get real traffic.
		const auto quantity =
			(rng() % 3) == 0 ? quantity_t{0}
							 : static_cast<quantity_t>(1 + (rng() % 500));

		b.update_level(side, price, quantity);

		if (quantity <= 0) {
			if (side == side_t::bid) {
				bids.erase(price);
			} else {
				asks.erase(price);
			}
		} else if (side == side_t::bid) {
			bids[price] = quantity;
		} else {
			asks[price] = quantity;
		}

		ASSERT_EQ(b.depth(side_t::bid), bids.size()) << "at update " << i;
		ASSERT_EQ(b.depth(side_t::ask), asks.size()) << "at update " << i;

		std::size_t index = 0;
		for (const auto &[p, q] : bids) {
			const auto &level = b.bid_levels()[index++];
			ASSERT_EQ(level.price, p) << "bid " << index << " at update " << i;
			ASSERT_EQ(level.volume, q) << "bid " << index << " at update " << i;
		}
		index = 0;
		for (const auto &[p, q] : asks) {
			const auto &level = b.ask_levels()[index++];
			ASSERT_EQ(level.price, p) << "ask " << index << " at update " << i;
			ASSERT_EQ(level.volume, q) << "ask " << index << " at update " << i;
		}
	}

	ASSERT_EQ(b.dropped_levels(), 0u) << "the window must not have evicted";
	EXPECT_EQ(b.update_count(), 4000u);
}

} // namespace
