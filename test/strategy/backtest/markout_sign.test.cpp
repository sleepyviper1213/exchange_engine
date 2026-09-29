#include "strategy/backtest/markout_recorder.hpp"
#include "strategy/backtest/markout_report.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

// The sign convention, which is the one thing here that can be wrong while
// every test still compiles and most still pass. Positive is *in our favour*:
// a buy profits when the mid rises above what we paid, a sell when it falls
// below what we sold at. Get it backwards and a strategy bleeding to adverse
// selection reports a profit, which is the exact failure a markout exists to
// catch.
//
// Everything is in half-ticks: the mid arrives as bid+ask rather than their
// average, so a one-tick move reads as 2. @see markout_report

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

constexpr std::uint64_t MARKOUT_SIGN_HORIZON = 100;

/// @brief A recorder measuring one short horizon, so a case is three calls.
markout_recorder sign_recorder() {
	static constexpr std::array<std::uint64_t, 1> ONE{MARKOUT_SIGN_HORIZON};
	return markout_recorder{std::span<const std::uint64_t>{ONE}};
}

/// @brief A two-sided book at @p bid / @p ask, doubled the way on_mid wants.
constexpr std::int64_t sign_mid(std::int64_t bid, std::int64_t ask) {
	return bid + ask;
}

} // namespace

TEST(MarkoutSign, ABuyIsPositiveWhenTheMidRisesAboveWhatWePaid) {
	auto recorder = sign_recorder();

	recorder.on_mid(0, sign_mid(100, 102));                    // mid 101
	recorder.on_fill(0, side_t::bid, 100, 5, /*is_passive=*/true);
	recorder.on_mid(MARKOUT_SIGN_HORIZON, sign_mid(104, 106)); // mid 105

	const markout_report out = recorder.finish();
	ASSERT_EQ(out.count, 1U);
	// Paid 100, mid moved to 105: five ticks good, ten half-ticks, times 5
	// lots.
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 50);
	EXPECT_EQ(passive_markout_per_lot(out.horizons[0]), 10);
}

TEST(MarkoutSign, ABuyIsNegativeWhenTheMidFallsBelowWhatWePaid) {
	auto recorder = sign_recorder();

	recorder.on_mid(0, sign_mid(100, 102));
	recorder.on_fill(0, side_t::bid, 102, 5, /*is_passive=*/true);
	recorder.on_mid(MARKOUT_SIGN_HORIZON, sign_mid(96, 98)); // mid 97

	const markout_report out = recorder.finish();
	// Paid 102, mid fell to 97: five ticks against us.
	EXPECT_EQ(passive_markout_per_lot(out.horizons[0]), -10);
}

TEST(MarkoutSign, ASaleIsPositiveWhenTheMidFallsBelowWhatWeSoldAt) {
	auto recorder = sign_recorder();

	recorder.on_mid(0, sign_mid(100, 102));
	recorder.on_fill(0, side_t::ask, 102, 3, /*is_passive=*/true);
	recorder.on_mid(MARKOUT_SIGN_HORIZON, sign_mid(96, 98)); // mid 97

	const markout_report out = recorder.finish();
	// Sold at 102, mid fell to 97: five ticks good.
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 30);
	EXPECT_EQ(passive_markout_per_lot(out.horizons[0]), 10);
}

TEST(MarkoutSign, ASaleIsNegativeWhenTheMidRisesAboveWhatWeSoldAt) {
	auto recorder = sign_recorder();

	recorder.on_mid(0, sign_mid(100, 102));
	recorder.on_fill(0, side_t::ask, 100, 3, /*is_passive=*/true);
	recorder.on_mid(MARKOUT_SIGN_HORIZON, sign_mid(104, 106)); // mid 105

	const markout_report out = recorder.finish();
	EXPECT_EQ(passive_markout_per_lot(out.horizons[0]), -10);
}

TEST(MarkoutSign, AnOddSpreadKeepsItsHalfTick) {
	auto recorder = sign_recorder();

	recorder.on_mid(0, sign_mid(100, 101));
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);
	// Mid 100.5 - off the grid, and the whole reason the unit is a half-tick.
	recorder.on_mid(MARKOUT_SIGN_HORIZON, sign_mid(100, 101));

	const markout_report out = recorder.finish();
	// Half a tick above what we paid, exactly, with no rounding to hide it.
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 1);
}

TEST(MarkoutSign, PassiveAndAggressiveAreAccumulatedApart) {
	auto recorder = sign_recorder();

	recorder.on_mid(0, sign_mid(100, 102));
	recorder.on_fill(0, side_t::bid, 100, 2, /*is_passive=*/true);
	recorder.on_fill(0, side_t::bid, 100, 7, /*is_passive=*/false);
	recorder.on_mid(MARKOUT_SIGN_HORIZON, sign_mid(104, 106));

	const markout_report out = recorder.finish();
	EXPECT_EQ(out.horizons[0].passive_fills, 1U);
	EXPECT_EQ(out.horizons[0].aggressive_fills, 1U);
	EXPECT_EQ(out.horizons[0].passive_lots, 2);
	EXPECT_EQ(out.horizons[0].aggressive_lots, 7);
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 20);
	EXPECT_EQ(out.horizons[0].aggressive_half_tick_lots, 70);
}

TEST(MarkoutSign, AFillOfNoSizeIsNotRecorded) {
	auto recorder = sign_recorder();

	recorder.on_mid(0, sign_mid(100, 102));
	recorder.on_fill(0, side_t::bid, 100, 0, /*is_passive=*/true);
	recorder.on_mid(MARKOUT_SIGN_HORIZON, sign_mid(104, 106));

	const markout_report out = recorder.finish();
	EXPECT_EQ(resolved_fills(out.horizons[0]), 0U);
	EXPECT_EQ(out.horizons[0].unresolved, 0U);
}
