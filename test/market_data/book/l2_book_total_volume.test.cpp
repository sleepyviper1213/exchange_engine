#include "market_data/l2_book.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

using namespace exchange;
using exchange::side_t;
using exchange::market_data::l2_book;
using total_volume_level = exchange::market_data::l2_book::price_level;

namespace {

/// @brief Levels per side in @c total_volume_deep - past any vector register
///        width, so the sum runs the blocked loop and not only its tail.
constexpr std::size_t TOTAL_VOLUME_DEEP_LEVELS = 77;

/// @brief A book whose ask side holds @c TOTAL_VOLUME_DEEP_LEVELS levels of a
///        distinct size each, so a sum that drops or double-counts one level
///        cannot come out right by accident.
l2_book total_volume_deep() {
	l2_book book{TOTAL_VOLUME_DEEP_LEVELS};
	for (std::size_t step = 0; step < TOTAL_VOLUME_DEEP_LEVELS; ++step) {
		const auto at = static_cast<std::int64_t>(step);
		book.set_level(side_t::ask,
					   at_scaled(1000 + at),
					   (at + 1) * units::scaled_size);
	}
	return book;
}

TEST(L2BookTotalVolume, AnEmptySideHoldsNothing) {
	const l2_book book;
	EXPECT_EQ(book.total_volume(side_t::bid), 0 * units::scaled_size);
	EXPECT_EQ(book.total_volume(side_t::ask), 0 * units::scaled_size);
}

TEST(L2BookTotalVolume, SumsTheSizesAndNotThePrices) {
	// Prices far larger than the sizes: a sum that picked up a price lane
	// instead of a size lane would be off by orders of magnitude rather than
	// subtly, which is the failure worth making obvious.
	l2_book book;
	book.set_level(side_t::ask,
				   at_scaled(1'000'000'000),
				   3 * units::scaled_size);
	book.set_level(side_t::ask,
				   at_scaled(1'000'000'001),
				   4 * units::scaled_size);
	EXPECT_EQ(book.total_volume(side_t::ask), 7 * units::scaled_size);
}

TEST(L2BookTotalVolume, TheTwoSidesAreCountedSeparately) {
	l2_book book;
	book.set_level(side_t::bid, at_scaled(99), 5 * units::scaled_size);
	book.set_level(side_t::ask, at_scaled(100), 11 * units::scaled_size);

	EXPECT_EQ(book.total_volume(side_t::bid), 5 * units::scaled_size);
	EXPECT_EQ(book.total_volume(side_t::ask), 11 * units::scaled_size);
}

TEST(L2BookTotalVolume, ALevelRemovedNoLongerCounts) {
	l2_book book;
	book.set_level(side_t::bid, at_scaled(99), 5 * units::scaled_size);
	book.set_level(side_t::bid, at_scaled(98), 6 * units::scaled_size);
	ASSERT_EQ(book.total_volume(side_t::bid), 11 * units::scaled_size);

	book.set_level(side_t::bid,
				   at_scaled(98),
				   0 * units::scaled_size); // a zero size removes the price
	EXPECT_EQ(book.total_volume(side_t::bid), 5 * units::scaled_size);
}

TEST(L2BookTotalVolume, OverwritingALevelReplacesItsSizeRatherThanAddingTo) {
	l2_book book;
	book.set_level(side_t::ask, at_scaled(100), 5 * units::scaled_size);
	book.set_level(side_t::ask, at_scaled(100), 8 * units::scaled_size);
	EXPECT_EQ(book.total_volume(side_t::ask), 8 * units::scaled_size);
}

// The vectorised path proper: enough levels to fill several registers, and
// sizes 1..n so the answer is a closed form the test can state independently
// of the loop that produces it.
TEST(L2BookTotalVolume, DeepSideSumsEveryLevel) {
	const l2_book book = total_volume_deep();
	constexpr scaled_qty_t EXPECTED =
		static_cast<std::int64_t>(TOTAL_VOLUME_DEEP_LEVELS *
								  (TOTAL_VOLUME_DEEP_LEVELS + 1) / 2) *
		units::scaled_size;

	ASSERT_EQ(book.depth(side_t::ask), TOTAL_VOLUME_DEEP_LEVELS);
	EXPECT_EQ(book.total_volume(side_t::ask), EXPECTED);
}

TEST(L2BookTotalVolume, AgreesWithSweepingTheWholeSide) {
	// Two independent readers of the same depth: the sum of every level, and
	// what an unbounded sweep manages to fill. They disagree only if one of
	// them is wrong about the retained window.
	const l2_book book = total_volume_deep();
	const scaled_qty_t held = book.total_volume(side_t::ask);

	EXPECT_EQ(book.sweep_asks(held).filled, held);
	EXPECT_TRUE(book.sweep_asks(held).is_complete());
	EXPECT_EQ(book.sweep_asks(held).levels, TOTAL_VOLUME_DEEP_LEVELS);
	EXPECT_FALSE(book.sweep_asks(held + 1 * units::scaled_size).is_complete());
}

TEST(L2BookTotalVolume, CountsOnlyTheRetainedWindow) {
	// The cap made visible, as everywhere else on this class: a book keeping
	// two levels reports the two it kept, not the three the venue published.
	l2_book book(2);
	const std::array<total_volume_level, 3> asks{
		total_volume_level{.price = at_scaled(100),
						   .qty   = 10 * units::scaled_size},
		total_volume_level{.price = at_scaled(101),
						   .qty   = 10 * units::scaled_size},
		total_volume_level{.price = at_scaled(102),
						   .qty   = 10 * units::scaled_size}};
	book.load(side_t::ask, asks);

	EXPECT_EQ(book.total_volume(side_t::ask), 20 * units::scaled_size);
	EXPECT_GT(book.dropped_levels(), 0U);
}

} // namespace
