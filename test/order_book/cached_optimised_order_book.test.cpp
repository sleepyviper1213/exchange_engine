#include "order_book/cached_optimised_order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <vector>

using namespace exchange;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::experimental::cached_optimised_order_book;

namespace {

/// The depth most cases use: deep enough that nothing is evicted, so a case
/// that is not about the capacity never trips over it.
using cached_ladder_book = cached_optimised_order_book<8>;

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
	const cached_ladder_book b;
	EXPECT_FALSE(b.best_bid().has_value());
	EXPECT_FALSE(b.best_ask().has_value());
	EXPECT_FALSE(b.spread().has_value());
	EXPECT_FALSE(b.is_crossed());
	EXPECT_TRUE(b.is_empty());
	EXPECT_EQ(b.size(), 0u);
	EXPECT_EQ(b.depth(side_t::bid), 0u);
	EXPECT_EQ(b.depth(side_t::ask), 0u);
	EXPECT_EQ(b.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
	EXPECT_EQ(b.dropped_levels(), 0u);
	EXPECT_EQ(b.update_count(), 0u);
}

TEST(CachedOptimisedOrderBook, MaxDepthIsTheTemplateArgumentAndIsPerSide) {
	EXPECT_EQ(cached_ladder_book::max_depth(), 8u);
	EXPECT_EQ(cached_optimised_order_book<1>::max_depth(), 1u);

	cached_ladder_book b;
	for (price_t p = at_tick(100); p < at_tick(108); ++p)
		b.update_level(side_t::bid, p, 1 * units::lot);
	for (price_t p = at_tick(200); p < at_tick(208); ++p)
		b.update_level(side_t::ask, p, 1 * units::lot);
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
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 5 * units::lot);

	EXPECT_EQ(b.volume_at_price(at_tick(100), side_t::bid), 5 * units::lot);
	EXPECT_EQ(b.best_bid().value(), at_tick(100));
	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_FALSE(b.is_empty());
	// A price is per-side: the same price on the ask side is independent.
	EXPECT_EQ(b.volume_at_price(at_tick(100), side_t::ask), 0 * units::lot);
	EXPECT_EQ(b.depth(side_t::ask), 0u);
}

TEST(CachedOptimisedOrderBook, BidsDescendingBestIsHighest) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 1 * units::lot);
	b.update_level(side_t::bid,
				   at_tick(102),
				   1 * units::lot); // higher -> becomes best
	b.update_level(side_t::bid,
				   at_tick(101),
				   1 * units::lot); // lands between the two

	EXPECT_EQ(b.best_bid().value(), at_tick(102));
	EXPECT_EQ(prices(b, side_t::bid),
			  (std::vector<price_t>{at_tick(102), at_tick(101), at_tick(100)}));
}

TEST(CachedOptimisedOrderBook, AsksAscendingBestIsLowest) {
	cached_ladder_book b;
	b.update_level(side_t::ask, at_tick(102), 1 * units::lot);
	b.update_level(side_t::ask,
				   at_tick(100),
				   1 * units::lot); // lower -> becomes best
	b.update_level(side_t::ask, at_tick(101), 1 * units::lot);

	EXPECT_EQ(b.best_ask().value(), at_tick(100));
	EXPECT_EQ(prices(b, side_t::ask),
			  (std::vector<price_t>{at_tick(100), at_tick(101), at_tick(102)}));
}

TEST(CachedOptimisedOrderBook, UpdateOnExistingPriceOverwritesVolume) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 5 * units::lot);
	b.update_level(side_t::bid, at_tick(100), 9 * units::lot);

	// Absolute size, not a delta, and no second level at the same price.
	EXPECT_EQ(b.volume_at_price(at_tick(100), side_t::bid), 9 * units::lot);
	EXPECT_EQ(b.depth(side_t::bid), 1u);
}

TEST(CachedOptimisedOrderBook, ZeroQuantityRemovesTheLevel) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(101), 5 * units::lot);
	b.update_level(side_t::bid, at_tick(100), 5 * units::lot);
	b.update_level(side_t::bid, at_tick(101), 0 * units::lot);

	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.volume_at_price(at_tick(101), side_t::bid), 0 * units::lot);
	EXPECT_EQ(b.best_bid().value(), at_tick(100)); // the touch moved down
}

TEST(CachedOptimisedOrderBook, NegativeQuantityRemovesTheLevel) {
	cached_ladder_book b;
	b.update_level(side_t::ask, at_tick(100), 5 * units::lot);
	b.update_level(side_t::ask, at_tick(100), -3 * units::lot);

	EXPECT_EQ(b.depth(side_t::ask), 0u);
	EXPECT_FALSE(b.best_ask().has_value());
}

TEST(CachedOptimisedOrderBook, RemovingAnAbsentPriceIsANoOp) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 5 * units::lot);
	b.update_level(side_t::bid,
				   at_tick(999),
				   0 * units::lot); // never rested here

	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.best_bid().value(), at_tick(100));
	EXPECT_EQ(b.dropped_levels(), 0u); // a no-op is not a dropped level
}

TEST(CachedOptimisedOrderBook, EraseFromTheMiddleKeepsTheSideSorted) {
	cached_ladder_book b;
	for (price_t p : {at_tick(100), at_tick(101), at_tick(102), at_tick(103)})
		b.update_level(side_t::bid, p, 1 * units::lot);
	b.update_level(side_t::bid, at_tick(102), 0 * units::lot);

	EXPECT_EQ(prices(b, side_t::bid),
			  (std::vector<price_t>{at_tick(103), at_tick(101), at_tick(100)}));
	EXPECT_EQ(b.volume_at_price(at_tick(101), side_t::bid), 1 * units::lot);
	EXPECT_EQ(b.volume_at_price(at_tick(103), side_t::bid), 1 * units::lot);
}

TEST(CachedOptimisedOrderBook, InsertIntoTheMiddleKeepsTheSideSorted) {
	cached_ladder_book b;
	for (price_t p : {at_tick(100), at_tick(102), at_tick(104)})
		b.update_level(side_t::ask, p, 1 * units::lot);
	b.update_level(side_t::ask, at_tick(103), 7 * units::lot);

	EXPECT_EQ(prices(b, side_t::ask),
			  (std::vector<price_t>{at_tick(100),
									at_tick(102),
									at_tick(103),
									at_tick(104)}));
	EXPECT_EQ(b.volume_at_price(at_tick(103), side_t::ask), 7 * units::lot);
	// The shift must carry the neighbours' volumes with them, not just prices.
	EXPECT_EQ(b.volume_at_price(at_tick(104), side_t::ask), 1 * units::lot);
}

TEST(CachedOptimisedOrderBook, ClearDropsBothSidesAndTheTouch) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 5 * units::lot);
	b.update_level(side_t::ask, at_tick(101), 5 * units::lot);
	b.clear();

	EXPECT_TRUE(b.is_empty());
	EXPECT_FALSE(b.best_bid().has_value());
	EXPECT_FALSE(b.best_ask().has_value());
	EXPECT_FALSE(b.spread().has_value());

	// Storage is reusable afterwards, with no stale level showing through.
	b.update_level(side_t::bid, at_tick(50), 2 * units::lot);
	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.best_bid().value(), at_tick(50));
	EXPECT_EQ(b.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
}

// --------------------------------------------------------------------------
// The cached touch: spread and crossing
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, SpreadIsBestAskMinusBestBid) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 1 * units::lot);
	EXPECT_FALSE(b.spread().has_value()); // one side is not a spread

	b.update_level(side_t::ask, at_tick(105), 1 * units::lot);
	EXPECT_EQ(b.spread().value(), 5U * units::tick);
	EXPECT_FALSE(b.is_crossed());

	// The touch is cached, so it has to keep up with a better price arriving.
	b.update_level(side_t::bid, at_tick(103), 1 * units::lot);
	EXPECT_EQ(b.spread().value(), 2U * units::tick);

	// ...and with the best level being removed.
	b.update_level(side_t::bid, at_tick(103), 0 * units::lot);
	EXPECT_EQ(b.spread().value(), 5U * units::tick);
}

TEST(CachedOptimisedOrderBook, CrossedAndLockedBooksReportNoSpread) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(105), 1 * units::lot);
	b.update_level(side_t::ask, at_tick(100), 1 * units::lot);

	EXPECT_TRUE(b.is_crossed());
	// price_t is unsigned: a crossed book has no representable spread, and
	// nullopt is the answer rather than a wrapped one.
	EXPECT_FALSE(b.spread().has_value());

	cached_ladder_book locked;
	locked.update_level(side_t::bid, at_tick(100), 1 * units::lot);
	locked.update_level(side_t::ask, at_tick(100), 1 * units::lot);
	EXPECT_TRUE(locked.is_crossed());
	EXPECT_FALSE(locked.spread().has_value());
}

TEST(CachedOptimisedOrderBook, OneSidedBookIsNeverCrossed) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 1 * units::lot);
	EXPECT_FALSE(b.is_crossed());

	b.update_level(side_t::bid, at_tick(100), 0 * units::lot);
	b.update_level(side_t::ask, at_tick(100), 1 * units::lot);
	EXPECT_FALSE(b.is_crossed());
}

// --------------------------------------------------------------------------
// Fixed capacity
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, FullSideRefusesAPriceWorseThanEveryLevel) {
	cached_optimised_order_book<3> b;
	for (price_t p : {at_tick(105), at_tick(104), at_tick(103)})
		b.update_level(side_t::bid, p, 1 * units::lot);

	b.update_level(side_t::bid,
				   at_tick(100),
				   9 * units::lot); // worse than all three

	EXPECT_EQ(b.depth(side_t::bid), 3u);
	EXPECT_EQ(prices(b, side_t::bid),
			  (std::vector<price_t>{at_tick(105), at_tick(104), at_tick(103)}));
	EXPECT_EQ(b.volume_at_price(at_tick(100), side_t::bid), 0 * units::lot);
	EXPECT_EQ(b.dropped_levels(), 1u);
}

TEST(CachedOptimisedOrderBook, FullSideEvictsTheWorstLevelForABetterPrice) {
	cached_optimised_order_book<3> b;
	for (price_t p : {at_tick(105), at_tick(104), at_tick(103)})
		b.update_level(side_t::bid, p, 1 * units::lot);

	b.update_level(side_t::bid,
				   at_tick(106),
				   9 * units::lot); // better than all three

	EXPECT_EQ(b.depth(side_t::bid), 3u);
	EXPECT_EQ(prices(b, side_t::bid),
			  (std::vector<price_t>{at_tick(106), at_tick(105), at_tick(104)}));
	EXPECT_EQ(b.best_bid().value(), at_tick(106));
	EXPECT_EQ(b.volume_at_price(at_tick(103), side_t::bid),
			  0 * units::lot); // 103 was evicted
	EXPECT_EQ(b.dropped_levels(), 1u);
}

TEST(CachedOptimisedOrderBook, EvictionOnAFullSideNeverDropsTheTouch) {
	// The whole point of the top-N window: whatever is discarded, the best
	// price is retained, so a cross detected here is a real one.
	cached_optimised_order_book<2> b;
	b.update_level(side_t::ask, at_tick(100), 1 * units::lot);
	b.update_level(side_t::ask, at_tick(101), 1 * units::lot);
	b.update_level(side_t::ask,
				   at_tick(99),
				   1 * units::lot); // best ask, side already full

	EXPECT_EQ(b.best_ask().value(), at_tick(99));
	EXPECT_EQ(prices(b, side_t::ask),
			  (std::vector<price_t>{at_tick(99), at_tick(100)}));
	EXPECT_EQ(b.dropped_levels(), 1u);
}

TEST(CachedOptimisedOrderBook, OverwritingOnAFullSideDropsNothing) {
	cached_optimised_order_book<3> b;
	for (price_t p : {at_tick(105), at_tick(104), at_tick(103)})
		b.update_level(side_t::bid, p, 1 * units::lot);

	b.update_level(side_t::bid,
				   at_tick(104),
				   42 * units::lot); // already resting: no insert

	EXPECT_EQ(b.volume_at_price(at_tick(104), side_t::bid), 42 * units::lot);
	EXPECT_EQ(b.depth(side_t::bid), 3u);
	EXPECT_EQ(b.dropped_levels(), 0u);
}

TEST(CachedOptimisedOrderBook, CapacityIsPerSideNotShared) {
	cached_optimised_order_book<2> b;
	b.update_level(side_t::bid, at_tick(100), 1 * units::lot);
	b.update_level(side_t::bid, at_tick(99), 1 * units::lot);
	b.update_level(side_t::ask, at_tick(101), 1 * units::lot);
	b.update_level(side_t::ask, at_tick(102), 1 * units::lot);

	// A full bid side must not refuse anything on the ask side.
	EXPECT_EQ(b.depth(side_t::bid), 2u);
	EXPECT_EQ(b.depth(side_t::ask), 2u);
	EXPECT_EQ(b.dropped_levels(), 0u);
}

TEST(CachedOptimisedOrderBook, DepthOfOneStillTracksTheBestPrice) {
	cached_optimised_order_book<1> b;
	b.update_level(side_t::bid, at_tick(100), 1 * units::lot);
	EXPECT_EQ(b.best_bid().value(), at_tick(100));

	b.update_level(side_t::bid, at_tick(99), 1 * units::lot); // worse: refused
	EXPECT_EQ(b.best_bid().value(), at_tick(100));
	EXPECT_EQ(b.depth(side_t::bid), 1u);

	b.update_level(side_t::bid,
				   at_tick(101),
				   1 * units::lot); // better: evicts 100
	EXPECT_EQ(b.best_bid().value(), at_tick(101));
	EXPECT_EQ(b.depth(side_t::bid), 1u);
	EXPECT_EQ(b.dropped_levels(), 2u);

	b.update_level(side_t::bid, at_tick(101), 0 * units::lot);
	EXPECT_FALSE(b.best_bid().has_value());
}

// --------------------------------------------------------------------------
// Per-level running statistics
// --------------------------------------------------------------------------

TEST(CachedOptimisedOrderBook, LevelStatisticsAccumulateOverWrites) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 10 * units::lot);
	b.update_level(side_t::bid, at_tick(100), 20 * units::lot);
	b.update_level(side_t::bid, at_tick(100), 30 * units::lot);

	const auto level = b.level_at_price(at_tick(100), side_t::bid);
	ASSERT_TRUE(level.has_value());
	EXPECT_EQ(level->price, at_tick(100));
	EXPECT_EQ(level->volume, 30 * units::lot); // the last absolute size
	EXPECT_EQ(level->count, 3u);  // writes landed here, including the insert
	EXPECT_EQ(level->total_volume, 60u);
	EXPECT_EQ(level->avg_order_size, 20u);
}

TEST(CachedOptimisedOrderBook, LevelStatisticsRestartWhenALevelComesBack) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 10 * units::lot);
	b.update_level(side_t::bid,
				   at_tick(100),
				   0 * units::lot); // level leaves the book
	b.update_level(side_t::bid, at_tick(100), 4 * units::lot); // and returns

	const auto level = b.level_at_price(at_tick(100), side_t::bid);
	ASSERT_TRUE(level.has_value());
	EXPECT_EQ(level->count, 1u);
	EXPECT_EQ(level->total_volume, 4u);
}

TEST(CachedOptimisedOrderBook, LevelTimestampMarksTheLastWriteToThatLevel) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 1 * units::lot); // update 1
	b.update_level(side_t::bid, at_tick(101), 1 * units::lot); // update 2
	b.update_level(side_t::bid,
				   at_tick(100),
				   2 * units::lot); // update 3, back to the first level

	EXPECT_EQ(b.update_count(), 3u);
	EXPECT_EQ(b.level_at_price(at_tick(100), side_t::bid)->timestamp, 3u);
	EXPECT_EQ(b.level_at_price(at_tick(101), side_t::bid)->timestamp, 2u);
}

TEST(CachedOptimisedOrderBook, LevelAtPriceIsNulloptWhenNothingRests) {
	cached_ladder_book b;
	b.update_level(side_t::bid, at_tick(100), 1 * units::lot);
	EXPECT_FALSE(b.level_at_price(at_tick(101), side_t::bid).has_value());
	EXPECT_FALSE(b.level_at_price(at_tick(100), side_t::ask).has_value());
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
		const price_t price = at_tick(static_cast<price_t::rep>(
			1000 + (rng() % static_cast<unsigned>(distinct))));
		// A third of updates remove, so erase paths get real traffic.
		const quantity_t quantity =
			(rng() % 3) == 0
				? quantity_t{}
				: quantity_t{static_cast<quantity_t::rep>(1 + (rng() % 500)) *
							 units::lot};

		b.update_level(side, price, quantity);

		if (mp_units::is_lteq_zero(quantity))
			if (side == side_t::bid) {
				bids.erase(price);
			} else {
				asks.erase(price);
			}
		else if (side == side_t::bid) bids[price] = quantity;
		else asks[price] = quantity;

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
