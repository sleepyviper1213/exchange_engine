#include "market_data/l2_book.hpp"

#include <gtest/gtest.h>

using exchange::side_t;
using exchange::market_data::depth_sweep;
using exchange::market_data::l2_book;

namespace {

/// @brief A book with three levels a tick apart on each side, ten a level.
l2_book laddered() {
	l2_book book;
	for (int step = 0; step < 3; ++step) {
		book.set_level(side_t::ask, 100 + step, 10);
		book.set_level(side_t::bid, 99 - step, 10);
	}
	return book;
}

// --------------------------------------------------------------------------
// Degenerate shapes
// --------------------------------------------------------------------------

TEST(L2BookSweep, AnEmptySideSuppliesNothing) {
	const l2_book book;
	const depth_sweep sweep = book.sweep_asks(100);

	EXPECT_FALSE(sweep.has_liquidity());
	EXPECT_FALSE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 0);
	EXPECT_EQ(sweep.levels, 0U);
	EXPECT_EQ(sweep.requested, 100);
}

TEST(L2BookSweep, TakingNothingIsCompleteAndTouchesNoLevel) {
	const l2_book book     = laddered();
	const depth_sweep zero = book.sweep_asks(0);

	// "Take nothing" has an answer, and it is not an error: nothing was asked
	// for and nothing is missing.
	EXPECT_TRUE(zero.is_complete());
	EXPECT_FALSE(zero.has_liquidity());
	EXPECT_EQ(zero.filled, 0);

	// A negative request is nonsense rather than a request, and comes back the
	// same empty sweep - unsatisfied, because nothing can satisfy it.
	EXPECT_FALSE(book.sweep_asks(-5).is_complete());
	EXPECT_FALSE(book.sweep_asks(-5).has_liquidity());
}

// --------------------------------------------------------------------------
// Walking the ladder
// --------------------------------------------------------------------------

TEST(L2BookSweep, SizeInsideTheTouchNeverLeavesTheFrontLevel) {
	const l2_book book      = laddered();
	const depth_sweep sweep = book.sweep_asks(4);

	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 4);
	EXPECT_EQ(sweep.levels, 1U);
	EXPECT_EQ(sweep.touch, 100);
	EXPECT_EQ(sweep.last, 100);
	EXPECT_EQ(sweep.impact(), 0) << "a fill at the touch moves nothing";
}

TEST(L2BookSweep, ReachesThroughAsManyLevelsAsTheSizeNeeds) {
	const l2_book book      = laddered();
	const depth_sweep sweep = book.sweep_asks(25);

	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 25);
	EXPECT_EQ(sweep.levels, 3U);
	EXPECT_EQ(sweep.touch, 100);
	EXPECT_EQ(sweep.last, 102);
	EXPECT_EQ(sweep.impact(), 2);
}

TEST(L2BookSweep, ASizeThatEndsOnALevelBoundaryDoesNotTouchTheNext) {
	// The off-by-one this walk is most likely to get wrong: 20 lots is exactly
	// the first two levels, so the third must be untouched and the last price
	// must be the second level's, not the third's.
	const l2_book book      = laddered();
	const depth_sweep sweep = book.sweep_asks(20);

	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.levels, 2U);
	EXPECT_EQ(sweep.last, 101);
	EXPECT_EQ(sweep.impact(), 1);
}

TEST(L2BookSweep, RunsOutOfDepthRatherThanInventingIt) {
	const l2_book book      = laddered();
	const depth_sweep sweep = book.sweep_asks(1000);

	EXPECT_FALSE(sweep.is_complete());
	EXPECT_TRUE(sweep.has_liquidity());
	EXPECT_EQ(sweep.requested, 1000);
	EXPECT_EQ(sweep.filled, 30) << "only what the window actually holds";
	EXPECT_EQ(sweep.levels, 3U);
	EXPECT_EQ(sweep.last, 102);
}

// --------------------------------------------------------------------------
// The seller's side, where prices get worse downwards
// --------------------------------------------------------------------------

TEST(L2BookSweep, ImpactIsNonNegativeOnBothSides) {
	const l2_book book     = laddered();
	const depth_sweep sell = book.sweep_bids(15);

	EXPECT_EQ(sell.side, side_t::bid);
	EXPECT_TRUE(sell.is_complete());
	EXPECT_EQ(sell.touch, 99);
	EXPECT_EQ(sell.last, 98) << "a seller walks down the bids";
	EXPECT_EQ(sell.impact(), 1) << "worse, not lower - the sign is resolved";
	EXPECT_EQ(book.sweep_asks(15).impact(), 1) << "and symmetrically for a buy";
}

TEST(L2BookSweep, TheTwoSidesAreIndependent) {
	l2_book book;
	book.set_level(side_t::ask, 100, 10);

	EXPECT_TRUE(book.sweep_asks(10).is_complete());
	EXPECT_FALSE(book.sweep_bids(10).has_liquidity());
}

// --------------------------------------------------------------------------
// Uneven depth, which is what real books look like
// --------------------------------------------------------------------------

TEST(L2BookSweep, AThinTouchInFrontOfSizeStillReportsTheWholeReach) {
	// One lot at the touch and a wall behind it: the impact of getting 50 lots
	// is set by where the wall is, not by how close the touch was.
	l2_book book;
	book.set_level(side_t::ask, 100, 1);
	book.set_level(side_t::ask, 110, 100);

	const depth_sweep sweep = book.sweep_asks(50);
	EXPECT_TRUE(sweep.is_complete());
	EXPECT_EQ(sweep.levels, 2U);
	EXPECT_EQ(sweep.touch, 100);
	EXPECT_EQ(sweep.last, 110);
	EXPECT_EQ(sweep.impact(), 10);
}

TEST(L2BookSweep, ACappedWindowReportsPessimisticImpactNotAnError) {
	// A book retaining two levels per side cannot know about the third, so a
	// size that the venue could fill comes back incomplete. That is the cap
	// being visible rather than a wrong answer - and dropped_levels says so.
	l2_book book(2);
	book.set_level(side_t::ask, 100, 10);
	book.set_level(side_t::ask, 101, 10);
	book.set_level(side_t::ask, 102, 10);

	const depth_sweep sweep = book.sweep_asks(25);
	EXPECT_FALSE(sweep.is_complete());
	EXPECT_EQ(sweep.filled, 20);
	EXPECT_EQ(sweep.levels, 2U);
	EXPECT_GT(book.dropped_levels(), 0U);
}

} // namespace
