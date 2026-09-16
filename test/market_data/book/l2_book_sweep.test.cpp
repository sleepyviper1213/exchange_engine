#include "market_data/l2_book.hpp"

#include <gtest/gtest.h>

#include <cstddef>

using exchange::side_t;
using exchange::market_data::depth_sweep;
using exchange::market_data::l2_book;

namespace {

/// @brief Levels per side in @c deep_laddered - comfortably past any vector
///        register width, so a sweep through it runs the blocked scan rather
///        than falling straight into the scalar tail.
///
/// The suites above this line all use three-level books, which means every one
/// of them exercises only the tail: they would pass unchanged against a purely
/// scalar sweep. The deep cases at the bottom are the ones that actually cross
/// a register boundary, and they are here because @c l2_book::sweep now walks
/// the ladder through @c core::simd::consume_interleaved.
constexpr std::size_t SWEEP_DEEP_LEVELS = 100;

/// @brief A book with three levels a tick apart on each side, ten a level.
l2_book laddered() {
	l2_book book;
	for (int step = 0; step < 3; ++step) {
		book.set_level(side_t::ask, 100 + step, 10);
		book.set_level(side_t::bid, 99 - step, 10);
	}
	return book;
}

/// @brief A book deep enough that a sweep crosses several vector registers:
///        @c SWEEP_DEEP_LEVELS asks a tick apart, ten a level.
l2_book deep_laddered() {
	l2_book book{SWEEP_DEEP_LEVELS};
	for (std::size_t step = 0; step < SWEEP_DEEP_LEVELS; ++step)
		book.set_level(side_t::ask,
					   100 + static_cast<exchange::market_data::scaled_price_t>(
								 step),
					   10);
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

// --------------------------------------------------------------------------
// Deep ladders - the blocked scan rather than the scalar tail
// --------------------------------------------------------------------------

// Every boundary in a ladder deep enough to cross several registers. A blocked
// scan that mishandles a block summing to exactly what is left reports one
// level too many or too few, and the failure is invisible on a three-level
// book because such a book never fills a register.
TEST(L2BookSweep, DeepLadderCountsLevelsAtEveryBoundary) {
	const l2_book book = deep_laddered();

	for (std::size_t level = 1; level <= SWEEP_DEEP_LEVELS; ++level) {
		const auto exact = static_cast<exchange::market_data::scaled_qty_t>(
			level * 10);

		// Ending exactly on a level stops after it, not before the next one.
		const depth_sweep on = book.sweep_asks(exact);
		EXPECT_EQ(on.levels, level) << "exactly " << exact;
		EXPECT_EQ(on.filled, exact);
		EXPECT_TRUE(on.is_complete());

		// One lot more reaches into the level behind it.
		if (level < SWEEP_DEEP_LEVELS) {
			const depth_sweep over = book.sweep_asks(exact + 1);
			EXPECT_EQ(over.levels, level + 1) << "one past " << exact;
			EXPECT_EQ(over.filled, exact + 1);
		}
	}
}

TEST(L2BookSweep, DeepLadderRunsOutAtTheBackOfTheWindow) {
	const l2_book book = deep_laddered();
	const auto held    = static_cast<exchange::market_data::scaled_qty_t>(
        SWEEP_DEEP_LEVELS * 10);

	const depth_sweep sweep = book.sweep_asks(held + 500);
	EXPECT_EQ(sweep.levels, SWEEP_DEEP_LEVELS);
	EXPECT_EQ(sweep.filled, held);
	EXPECT_EQ(sweep.requested - sweep.filled, 500);
	EXPECT_FALSE(sweep.is_complete());
}

} // namespace
