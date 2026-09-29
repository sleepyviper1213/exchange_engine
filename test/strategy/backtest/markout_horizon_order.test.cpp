#include "strategy/backtest/markout_recorder.hpp"
#include "strategy/backtest/markout_report.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

// Why the recorder keeps one queue per horizon rather than one queue of fills.
//
// With horizons far apart, a *later* fill's next deadline can fall before an
// earlier fill's. A single queue drained from the front would stop at the
// earlier fill - whose next deadline is the long one, not yet due - and never
// look at the later fill sitting behind it, whose short horizon *is* due.
//
// What failure looks like: TheShortHorizonOfALaterFillIsNotBlocked would report
// one resolved fill instead of two, and the missing one would surface as
// `unresolved` at the end of a run rather than as an obvious error.

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

/// @brief Deliberately far apart, which is what creates the interleaving.
constexpr std::array<std::uint64_t, 2> HORIZON_ORDER_SPREAD{10, 1000};

markout_recorder spread_recorder() {
	return markout_recorder{
		std::span<const std::uint64_t>{HORIZON_ORDER_SPREAD}};
}

} // namespace

TEST(MarkoutHorizonOrder, TheShortHorizonOfALaterFillIsNotBlocked) {
	auto recorder = spread_recorder();

	recorder.on_mid(0, 100 + 100);
	// Fill A at 0:  short due at 10,  long due at 1000.
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);

	// A's short horizon resolves here; its long one is now the head of a
	// single-queue design and is not due until 1000.
	recorder.on_mid(11, 100 + 100);

	// Fill B at 11: short due at 21 - far sooner than A's outstanding 1000.
	recorder.on_fill(11, side_t::bid, 100, 1, /*is_passive=*/true);
	recorder.on_mid(30, 104 + 104);

	const markout_report out = recorder.finish();
	ASSERT_EQ(out.count, 2U);
	// Both short horizons priced: A's at t=10 and B's at t=21.
	EXPECT_EQ(resolved_fills(out.horizons[0]), 2U);
	// Neither long one has come due.
	EXPECT_EQ(resolved_fills(out.horizons[1]), 0U);
	EXPECT_EQ(out.horizons[1].unresolved, 2U);
}

TEST(MarkoutHorizonOrder, EachHorizonPricesTheSameFillSeparately) {
	auto recorder = spread_recorder();

	recorder.on_mid(0, 100 + 100);
	recorder.on_fill(0, side_t::bid, 100, 1, /*is_passive=*/true);

	recorder.on_mid(10, 102 + 102); // short horizon: +2 ticks
	recorder.on_mid(1000, 90 + 90); // long horizon: -10 ticks

	const markout_report out = recorder.finish();
	// The curve turning over is the whole point: good at 10ns, bad at 1000ns
	// is exactly the shape adverse selection makes.
	EXPECT_EQ(out.horizons[0].passive_half_tick_lots, 4);
	EXPECT_EQ(out.horizons[1].passive_half_tick_lots, -20);
}

TEST(MarkoutHorizonOrder, HorizonsAreReportedInTheOrderTheyWereGiven) {
	auto recorder = spread_recorder();

	const markout_report out = recorder.finish();
	ASSERT_EQ(out.count, 2U);
	EXPECT_EQ(out.horizons[0].horizon_ns, 10U);
	EXPECT_EQ(out.horizons[1].horizon_ns, 1000U);
}

TEST(MarkoutHorizonOrder, TheDefaultCurveHasSixHorizons) {
	const markout_recorder recorder;

	EXPECT_EQ(recorder.horizon_count(), 6U);
	const markout_report out = recorder.finish();
	ASSERT_EQ(out.count, 6U);
	EXPECT_EQ(out.horizons[0].horizon_ns, 10'000'000U);
	EXPECT_EQ(out.horizons[5].horizon_ns, 5'000'000'000U);
}
