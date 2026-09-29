#include "strategy/backtest/markout_recorder.hpp"
#include "strategy/backtest/markout_report.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

// The rule the whole measurement rests on: a horizon is scored against the last
// mid at or *before* its deadline, never the first one after it.
//
// This matters because a venue diff feed is not continuous. On a 100ms cadence
// a 10ms horizon almost never lands on a frame - it lands between two of them,
// and the next frame carries 90ms of information from after the horizon closed.
// Scoring against it would mean a markout that "predicts" moves it could not
// have seen, which is look-ahead bias wearing the costume of a result.
//
// What failure looks like: AHorizonBetweenFramesTakesTheEarlierFrame would read
// 20 instead of 0 - the later frame's move leaking backwards into a window that
// closed before it.

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

constexpr std::uint64_t LOOK_AHEAD_HORIZON = 10;

markout_recorder look_ahead_recorder() {
	static constexpr std::array<std::uint64_t, 1> ONE{LOOK_AHEAD_HORIZON};
	return markout_recorder{std::span<const std::uint64_t>{ONE}};
}

} // namespace

TEST(MarkoutLookAhead, AHorizonBetweenFramesTakesTheEarlierFrame) {
	auto recorder = look_ahead_recorder();

	// Frame at 0: mid 100. Fill at 0, so the 10ns horizon is due at 10.
	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);

	// The next frame is at 50 - long after the horizon closed - and the market
	// moved ten ticks in it. None of that is knowable at t=10.
	recorder.on_mid(50, 110 + 110);

	const markout_report out = recorder.finish();
	ASSERT_EQ(resolved_fills(out.horizons[0]), 1U);
	// Prevailing mid at t=10 was still 100, which is what we paid.
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 0);
}

TEST(MarkoutLookAhead, AHorizonLandingExactlyOnAFrameTakesThatFrame) {
	auto recorder = look_ahead_recorder();

	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);
	// Due at exactly 10, and a frame arrives at exactly 10. That frame is the
	// last observation at-or-before the deadline, so it counts.
	recorder.on_mid(LOOK_AHEAD_HORIZON, 106 + 106);

	const markout_report out = recorder.finish();
	ASSERT_EQ(resolved_fills(out.horizons[0]), 1U);
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 12);
}

TEST(MarkoutLookAhead, AStaleMidCarriesForwardAcrossASilentGap) {
	auto recorder = look_ahead_recorder();

	recorder.on_mid(0, 100 + 100);
	// Fill long after the last frame: the book has not moved since t=0, so the
	// prevailing mid at the fill and at its deadline is still 100.
	recorder.on_fill(1000, side_t::bid, 100, 1, /*is_passive=*/true);
	recorder.on_mid(5000, 120 + 120);

	const markout_report out = recorder.finish();
	ASSERT_EQ(resolved_fills(out.horizons[0]), 1U);
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 0);
}

TEST(MarkoutLookAhead, TheFrameAFillPrintsInDoesNotScoreIt) {
	auto recorder = look_ahead_recorder();

	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);
	// A second frame at the fill's own instant. The horizon is not due until
	// 10, so this must not resolve anything - otherwise every fill would be
	// scored at zero elapsed time and the curve would be flat by construction.
	recorder.on_mid(0, 130 + 130);

	EXPECT_EQ(recorder.pending(), 1U);
	const markout_report out = recorder.finish();
	EXPECT_EQ(resolved_fills(out.horizons[0]), 0U);
	EXPECT_EQ(out.horizons[0].unresolved, 1U);
}
