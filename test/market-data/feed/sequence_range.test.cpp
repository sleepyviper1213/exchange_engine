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

// sequence_range — the one sequencing primitive every venue maps onto.

namespace {

TEST(SequenceRange, CoversItsClosedInterval) {
	constexpr sequence_range range{10, 12};
	EXPECT_FALSE(range.covers(9));
	EXPECT_TRUE(range.covers(10)); // both ends are inclusive
	EXPECT_TRUE(range.covers(11));
	EXPECT_TRUE(range.covers(12));
	EXPECT_FALSE(range.covers(13));
}

TEST(SequenceRange, ASingleNumberIsADegenerateRange) {
	constexpr sequence_range range{7, 7};
	EXPECT_TRUE(range.ordered());
	EXPECT_TRUE(range.covers(7));
}

TEST(SequenceRange, ABackwardsRangeIsNotOrdered) {
	EXPECT_TRUE((sequence_range{4, 9}).ordered());
	EXPECT_FALSE((sequence_range{9, 4}).ordered());
	// A backwards range covers nothing, so a caller that skips the check still
	// cannot conclude anything from it.
	EXPECT_FALSE((sequence_range{9, 4}).covers(6));
}

TEST(SequenceRange, ComparesFieldwise) {
	EXPECT_EQ((sequence_range{1, 2}), (sequence_range{1, 2}));
	EXPECT_NE((sequence_range{1, 2}), (sequence_range{1, 3}));
}

} // namespace
