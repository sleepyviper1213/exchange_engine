#include "backtest.fixture.hpp"
#include "strategy/backtest/format.hpp"
#include "strategy/backtest/markout_report.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

// What the curve looks like when a person reads it. Worth pinning for one
// reason beyond cosmetics: the printed number is a *different* computation from
// the stored one - stored is half-ticks size-weighted, printed is ticks per lot
// - and a mistake in the halving would show up here and nowhere else.

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

/// @brief A curve with one horizon, filled in by the caller.
markout_report one_horizon_curve(markout_report::bucket at) {
	markout_report out;
	out.count       = 1;
	out.horizons[0] = at;
	return out;
}

std::string rendered(const markout_report &curve,
					 const engine::symbol_spec &spec) {
	return fmt::format("{}", markout_summary{&curve, &spec});
}

} // namespace

TEST(MarkoutFormat, SaysSoWhenNothingWasRecorded) {
	const engine::symbol_spec spec = unit_listing();
	const markout_report empty;

	const std::string text = rendered(empty, spec);
	EXPECT_NE(text.find("(not recorded)"), std::string::npos) << text;
}

TEST(MarkoutFormat, PrintsTicksPerLotAndNotTheStoredHalfTicks) {
	const engine::symbol_spec spec = unit_listing();
	// Twenty half-ticks over five lots is -2.0 ticks per lot.
	const markout_report curve = one_horizon_curve(
		{.horizon_ns             = 10'000'000,
		 .passive_fills          = 1,
		 .passive_lots           = 5 * units::lot,
		 .passive_half_tick_lots = -20 * (units::half_tick * units::lot)});

	const std::string text = rendered(curve, spec);
	EXPECT_NE(text.find("10ms"), std::string::npos) << text;
	EXPECT_NE(text.find("-2.0"), std::string::npos) << text;
}

TEST(MarkoutFormat, KeepsAHalfTickInTheDecimal) {
	const engine::symbol_spec spec = unit_listing();
	// One half-tick over one lot is exactly half a tick, and printing it as
	// "0.0" or "1.0" would be the rounding the half-tick unit exists to avoid.
	const markout_report curve = one_horizon_curve(
		{.horizon_ns             = 1'000'000'000,
		 .passive_fills          = 1,
		 .passive_lots           = 1 * units::lot,
		 .passive_half_tick_lots = 1 * (units::half_tick * units::lot)});

	const std::string text = rendered(curve, spec);
	EXPECT_NE(text.find("0.5"), std::string::npos) << text;
	EXPECT_NE(text.find("1s"), std::string::npos) << text;
}

TEST(MarkoutFormat, MarksAHorizonMostFillsOutlived) {
	const engine::symbol_spec spec = unit_listing();
	const markout_report thin      = one_horizon_curve(
		{.horizon_ns             = 5'000'000'000,
		 .passive_fills          = 1,
		 .passive_lots           = 1 * units::lot,
		 .passive_half_tick_lots = 2 * (units::half_tick * units::lot),
		 .unresolved             = 9});

	const std::string text = rendered(thin, spec);
	EXPECT_NE(text.find("thin"), std::string::npos)
		<< "a bucket where nine of ten fills never resolved must say so:\n"
		<< text;
}

TEST(MarkoutFormat, DoesNotCallAnEmptyHorizonThin) {
	const engine::symbol_spec spec = unit_listing();
	// No fills at all. `is_covered` is false here - correctly, there is nothing
	// to be covered of - but the thin marker asserts that fills outlived the
	// run, and none existed. A run that never traded must not be told its
	// sample is biased.
	const markout_report curve = one_horizon_curve({.horizon_ns = 10'000'000});

	const std::string text = rendered(curve, spec);
	EXPECT_EQ(text.find("thin"), std::string::npos)
		<< "a horizon with no fills claimed a biased sample:\n"
		<< text;
}

TEST(MarkoutFormat, LeavesAnEmptySideAsADashRatherThanAZero) {
	const engine::symbol_spec spec = unit_listing();
	// Passive only. A zero in the aggressive column would read as "we crossed
	// and it cost nothing", which is a claim; a dash is the absence of one.
	const markout_report curve = one_horizon_curve(
		{.horizon_ns             = 10'000'000,
		 .passive_fills          = 3,
		 .passive_lots           = 3 * units::lot,
		 .passive_half_tick_lots = 6 * (units::half_tick * units::lot)});

	const std::string text = rendered(curve, spec);
	EXPECT_NE(text.find('-'), std::string::npos) << text;
}
