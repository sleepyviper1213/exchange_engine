#include "strategy/backtest/markout_recorder.hpp"
#include "strategy/backtest/markout_report.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

// A capture ends; the market does not. Fills near the end have horizons the run
// never reached, and the tempting thing is to score them against the last frame
// that happened to exist. That would be a shorter horizon reported under a
// longer one's name - and worse, it would bias the long buckets systematically,
// because the fills it invents are always the ones with the least time to move.
//
// So they are counted and not scored, and `is_covered` exists so a reader can
// tell a 5s bucket built from the whole run from one built from its first half.

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

constexpr std::array<std::uint64_t, 2> UNRESOLVED_HORIZONS{10, 100};

markout_recorder unresolved_recorder() {
	return markout_recorder{
		std::span<const std::uint64_t>{UNRESOLVED_HORIZONS}};
}

} // namespace

TEST(MarkoutUnresolved, AHorizonTheCaptureNeverReachedIsCountedNotScored) {
	auto recorder = unresolved_recorder();

	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 4, /*is_passive=*/true);
	// Reaches the 10ns horizon and stops well short of the 100ns one.
	recorder.on_mid(20, 130 + 130);

	const markout_report out = recorder.finish();
	EXPECT_EQ(resolved_fills(out.horizons[0]), 1U);
	EXPECT_EQ(out.horizons[0].unresolved, 0U);

	EXPECT_EQ(resolved_fills(out.horizons[1]), 0U);
	EXPECT_EQ(out.horizons[1].unresolved, 1U);
	// Nothing invented: the last mid was 30 ticks up, and it is not in here.
	EXPECT_EQ(out.horizons[1].passive_half_tick_lots, 0);
	EXPECT_EQ(out.horizons[1].passive_lots, 0);
}

TEST(MarkoutUnresolved, FinishDoesNotConsumeThePending) {
	auto recorder = unresolved_recorder();

	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);

	const markout_report first = recorder.finish();
	EXPECT_EQ(first.horizons[1].unresolved, 1U);

	// The run continues past the first read, and the horizon then resolves.
	recorder.on_mid(100, 108 + 108);
	const markout_report second = recorder.finish();
	EXPECT_EQ(second.horizons[1].unresolved, 0U);
	EXPECT_EQ(second.horizons[1].passive_half_tick_lots, 16);
}

TEST(MarkoutUnresolved, CoverageIsFalseWhenMostFillsOutlivedTheCapture) {
	auto recorder = unresolved_recorder();

	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);
	recorder.on_mid(100, 100 + 100); // resolves the first fill at both horizons

	// Three more near the end. The run lasts long enough for their short
	// horizon and nowhere near long enough for their long one.
	recorder.on_fill(100, side_t::bid, 100, 1, /*is_passive=*/true);
	recorder.on_fill(101, side_t::bid, 100, 1, /*is_passive=*/true);
	recorder.on_fill(102, side_t::bid, 100, 1, /*is_passive=*/true);
	recorder.on_mid(150, 100 + 100);

	const markout_report out = recorder.finish();
	// Short horizon: all four priced, so the average speaks for the whole run.
	EXPECT_EQ(resolved_fills(out.horizons[0]), 4U);
	EXPECT_TRUE(is_covered(out.horizons[0]));

	// Long horizon: one of four, and that one is the earliest fill. An average
	// over it is an average over whichever fills had room to mature, which is
	// exactly the bias the flag is there to expose.
	EXPECT_EQ(resolved_fills(out.horizons[1]), 1U);
	EXPECT_EQ(out.horizons[1].unresolved, 3U);
	EXPECT_FALSE(is_covered(out.horizons[1]));
}

TEST(MarkoutUnresolved, CoverageIsFalseForAHorizonNothingReached) {
	auto recorder = unresolved_recorder();

	recorder.on_mid(0, 100 + 100);
	const markout_report out = recorder.finish();

	// No fills at all is not "covered" - there is nothing to be covered of.
	EXPECT_FALSE(is_covered(out.horizons[0]));
	EXPECT_FALSE(is_covered(out.horizons[1]));
}

TEST(MarkoutUnresolved, PendingTracksTheLongestHorizon) {
	auto recorder = unresolved_recorder();

	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);
	EXPECT_EQ(recorder.pending(), 1U);

	recorder.on_mid(10, 100 + 100);  // short horizon done, long one not
	EXPECT_EQ(recorder.pending(), 1U);

	recorder.on_mid(100, 100 + 100); // both done
	EXPECT_EQ(recorder.pending(), 0U);
}
